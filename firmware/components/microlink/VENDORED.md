# Vendored: MicroLink (Tailscale client for ESP-IDF)

MIT, Cameron Malone; see LICENSE and x25519-license.txt.

- Upstream: https://github.com/CamM2325/microlink
- Taken from the copy maintained in https://github.com/Csontikka/esphome-tailscale
  (`microlink/components/microlink/`), repo commit
  `a3067dd4dffaed39c9eda108ea4a4758ddb48b73`; last commit touching that
  directory `4bfb3a7`. That copy is based on upstream's dev line `058b374`
  and carries fixes upstream has not merged, most importantly the lwIP
  threading fix (upstream issues #15/#17, PR #28): the WireGuard netif's
  `input` is `tcpip_input` (`src/ml_wg_mgr.c`, `netif->input = tcpip_input`),
  so decrypted packets reach TCP on the TCPIP thread rather than from
  `ml_wg_mgr`. It also fixes the stop/destroy leaks and use-after-free that a
  reconnect hit.

Only built with `CONFIG_UPSA_TAILSCALE`; the glue is `../tailnet`.

## Copied

`src/`, `include/`, `Kconfig`, `CMakeLists.txt`, `LICENSE`, `x25519-license.txt`.

Left out: examples, docs, `ml_config_override.h` (ESPHome-only string defaults),
and the nested `components/wireguard_lwip`, which lives once, at
`../wireguard_lwip`.

## Local changes

Each is marked with a `local patch` comment in the source. To update: re-copy
the same files from a newer esphome-tailscale commit and reapply these.

Build and configuration:

- `Kconfig`: the whole menu is inside `if UPSA_TAILSCALE`, so a build without
  Tailscale has no `CONFIG_ML_*` symbols; `ML_ENABLE_CONFIG_HTTPD` is
  `depends on n`. That server binds port 80, which the web UI owns, and is an
  unauthenticated page that rewrites Wi-Fi and the auth key.
  `../tailnet/tailnet.c` also `#error`s if it is ever on.
- `CMakeLists.txt`: `SRCS` is emptied unless `CONFIG_UPSA_TAILSCALE`, so Rev A
  and plain Rev B compile none of it (REQUIRES stays unconditional: CONFIG_
  values are unknown when IDF first expands requirements). The
  PlatformIO-only `esp-tls` include hack is dropped; native IDF gets it from
  REQUIRES.
- Three comments that gave example routes in 192.168.x ranges use the
  documentation ranges 203.0.113.0/24 and 198.51.100.0/24. No code change.

Tasks and memory:

- `src/ml_derp.c`, DERP payload read loop: `vTaskDelay(pdMS_TO_TICKS(10))`,
  upstream 5. At this firmware's 100 Hz tick, 5 ms is 0 ticks, and the loop
  would spin at priority 5 and starve IDLE0 into the task watchdog. Upstream
  runs at ESPHome's 1000 Hz. This is the only sub-tick delay in the sources
  compiled here (cellular/AT-socket code is compiled out).
- `src/microlink.c`: `ml_net_io`, `ml_derp_tx`, `ml_derp_rx` and `ml_coord`
  are created by `ml_task_create_psram()` with their stacks in PSRAM, falling
  back to internal RAM, which keeps the internal heap free for Wi-Fi, lwIP and
  the USB host. None of the four writes flash (a task with a PSRAM stack must
  not: the cache is off while flash is written); `ml_wg_mgr` saves peers to
  NVS and keeps an internal stack.
- `include/microlink_internal.h`, `src/microlink.c`: those four tasks never
  delete themselves. A task created with caps cannot free its own stack, so
  for `vTaskDeleteWithCaps(NULL)` ESP-IDF creates a temporary helper task and
  calls `abort()` if that helper cannot be allocated, which on a board with
  little internal RAM left turns a Wi-Fi drop or a Tailscale restart into a
  reboot. Instead each task calls `ml_task_exiting()`, which sets its bit in
  `ml->tasks_parked` and drops `tasks_alive`, then parks in `ML_TASK_EXIT()`
  (`vTaskSuspend(NULL)`). `microlink_stop()` waits for `tasks_alive` to reach
  zero, then `ml_delete_parked()` calls `vTaskDeleteWithCaps(handle)` from the
  stopping task for each parked task; deleting another task allocates nothing.
  A task that never signed off (the stop wait timed out) is leaked with the
  instance, as `stop_incomplete` already did. The tasks only exit on
  `ML_EVT_SHUTDOWN_REQUEST`, which only `microlink_stop()` sets, and a failed
  `microlink_start()` is followed by `microlink_destroy()`, which stops first
  (`../tailnet/tailnet.c`). `ml_wg_mgr` is created without caps and still ends
  with `vTaskDelete(NULL)`, which needs no helper task.
- `include/microlink_internal.h`: `ML_TASK_WG_MGR_STACK` is 6 KB, upstream
  8 KB; its peak use is about 4.3 KB (comment above the stack sizes). It stays
  in internal RAM because it writes peers to NVS.
- `src/ml_net_io.c`, `src/ml_derp.c`: received datagram copies are allocated
  in PSRAM (`ml_psram_malloc`), like the queue payloads.
- `src/ml_peer_nvs.c`, `src/ml_wg_mgr.c`: the peer cache is marked dirty and
  written once per batch of peer updates (`ml_peer_nvs_flush`), and a peer
  re-sent unchanged is not written at all. Upstream wrote the whole blob
  (about 1.9 KB at 16 peers) once per peer.

Threading:

- `src/ml_wg_mgr.c`, "TCPIP-thread ownership": with
  `CONFIG_LWIP_TCPIP_CORE_LOCKING=n` the raw lwIP API is legal only on the
  TCPIP thread, and upstream called it from the wg_mgr task (the `netif_list`
  splice at bring-up, `netif_set_down`/`netif_remove` at stop, and
  `udp_sendto` for every keepalive, handshake initiation and response through
  `wireguardif_network_rx` and `wireguardif_periodic`). The same two threads
  shared `struct wireguard_device` with no lock. Now every `wireguardif_*`
  call that touches the device, the netif attach (output PCB, list splice,
  up, then `ml->wg_netif`), the address update (`netif_set_ipaddr`), the
  periodic tick, each received datagram and the teardown go through
  `wg_on_tcpip()`, a synchronous `tcpip_api_call()` whose message lives on the
  caller's stack: no allocation that could fail, the caller waits until it
  ran, and it is FIFO with everything lwIP posted to itself before it. The
  teardown is two such calls, detach then free, with `free(netif)` after the
  second, so no decrypted packet already queued for `tcpip_input()` can meet
  a freed netif. `wg_on_tcpip()` runs the function inline when the caller
  already is the TCPIP thread. `wg_udp_output_cb` checks the thread and drops
  the packet (logged once) rather than send from the wrong one; the
  `tcpip_callback_with_block()` bring-up calls, which never waited for
  completion, are gone. Handshake crypto (X25519, about 40 ms a peer)
  therefore runs on the TCPIP thread, as upstream's lazy initiation in
  `wireguardif_output` always did; the comment block at the top of the file
  gives the reasoning and the per-field thread ownership table.
  `sdkconfig.defaults.tailscale` raises the TCPIP task stack to 4096 for it.
  The matching check in the WireGuard code is in `../wireguard_lwip/VENDORED.md`.
- `src/ml_derp.c`, `src/microlink.c`, `include/microlink_internal.h`, DERP
  reader/writer teardown: the reader holds `derp.rx_lock` while it touches the
  TLS context or socket, re-checks `connected` on every retry of a payload
  read and leaves; the writer frees the context only while holding the lock
  (`derp_teardown`, 10 s bound). If the bound runs out nothing is freed: the
  writer falls into the normal reconnect backoff and `ml_derp_connect()`
  retries the teardown before it reinitialises anything. A PONG is queued to
  the writer instead of written from the reader. The comment that called
  concurrent read and write on one context mbedTLS's documented model is
  corrected: it is not; what is relied on is stated.

DERP and control plane:

- `src/ml_derp.c`: the relay's TLS certificate is verified
  (`MBEDTLS_SSL_VERIFY_REQUIRED` with `esp_crt_bundle_attach`); upstream did
  not verify it.
- `src/ml_derp.c`: the ServerInfo length read off the wire is capped at 4 KB
  (`DERP_SERVER_INFO_MAX`), and a short read of it fails the connect instead
  of desynchronising the stream.
- `src/ml_coord.c`, `src/ml_netcheck.c`: the DERP latency probe runs only when
  `netcheck_override_enabled` is on, and exits early once every probed region
  has answered. Upstream ran it on every DERP map and discarded the result,
  stalling the control task for up to 25 s.
- `src/microlink.c`: the TAI64N base is stored on every boot with a flag
  saying whether it may run ahead of real time, and picked by
  `ml_tai_base_pick()`: wall clock minus uptime, never below the stored base
  (a day above it if that one ran ahead); without a clock, stored + one day.

Logging:

- `src/ml_wg_mgr.c`, `src/ml_derp.c`: per-packet and every-10-s log lines are
  at DEBUG (WG UDP TX, WG RX, WG INIT, direct probe, WG_SNAP, DISCO counts,
  DERP HEARTBEAT and status); "DERP output cb: not connected" is logged once
  per 32 drops. This firmware logs at INFO on a 115200-baud console, from the
  TCPIP thread.
