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

- `src/ml_wg_mgr.c`, `src/ml_coord.c`, `include/ml_peer_evict.h`, GitHub #18:
  `CONFIG_ML_MAX_PEERS` is 64 in `sdkconfig.defaults.tailscale` (upstream
  default 16; both peer tables are PSRAM heap blocks). A peer refused for
  lack of room waits (`microlink_t.wait`, PSRAM, 256), and
  when it sends the board a DISCO packet it takes the slot of a peer that is
  offline, or idle for 10 minutes with no WireGuard session (never the
  priority peer or an exit node). A netmap peer that is online takes only an
  offline peer's slot. Upstream evicted only for the priority peer, so on a
  tailnet larger than the table the first 16 in netmap order held it for good.
  Every authoritative netmap first clears the waiting list (new action
  `ML_PEER_WAIT_CLEAR`), so a revoked peer cannot get back in by pinging, and
  `remove_peer` drops a waiting entry too. The authoritative sweep is queued
  before the adds, so a departed peer frees its slot before a new one asks.
  `tools/test/test_tailnet_peer_table.py`.
- `src/ml_wg_mgr.c`, `src/ml_derp.c`: per-packet and every-10-s log lines are
  at DEBUG (WG UDP TX, WG RX, WG INIT, direct probe, WG_SNAP, DISCO counts,
  DERP HEARTBEAT and status); "DERP output cb: not connected" is logged once
  per 32 drops. This firmware logs at INFO on a 115200-baud console, from the
  TCPIP thread.

Control plane:

- `include/microlink.h`, `include/microlink_internal.h`, GitLab #1:
  `MICROLINK_CAPABILITY_VERSION` (131) is public and `ML_CTRL_PROTOCOL_VER`
  is defined from it, so `../tailnet` can report a matching
  `Hostinfo.IPNVersion` and fail to build if the two drift.
- `src/ml_derp.c`, `src/ml_read_full.h`, GitLab #30: the relay reader reads
  the 5-byte frame header and the payload through `ml_read_full()`, which
  keeps reading across short reads. Upstream read the header with one
  `mbedtls_ssl_read()` and reconnected to the relay on 1 to 4 bytes, which a
  TLS record boundary inside a header causes. A close part way through a
  payload is now an error (it was reported as "no data"). Host test:
  `tools/test/test_ml_read_full.py`.
- `src/microlink.c`, `src/ml_coord.c`, `src/ml_derp.c`, `include/microlink.h`,
  GitLab #27: the control and relay TCP connects go through
  `ml_connect_abortable()` (non-blocking, `select()` every 250 ms, leaves on
  `ML_EVT_SHUTDOWN_REQUEST`, 20 s limit); coord's wait for the relay also ends
  on the shutdown bit; `microlink_stop()` waits 25 s, was 15. When a task
  still runs after that, `microlink_destroy()` now returns false and frees
  nothing, the parked-task handles are kept, and the caller frees the
  instance later with the new `microlink_reap()`, which leaves the
  process-wide peer cache to the newer instance. Upstream (and this tree
  before) dropped such an instance: about 7 KB internal and up to 600 KB
  PSRAM each time.
- `src/ml_wg_mgr.c`, review G-B7: the WireGuard output PCB's source port is
  set from `disco_local_port` on every attach (it was 51820 always, wrong
  whenever `microlink_start()` fell back to an ephemeral port).
- `src/ml_wg_mgr.c`, review G-B14: the hand-spliced netif gets a free
  `num` (it kept 0, the loopback's) and an `output_ip6` that returns
  `ERR_RTE` instead of NULL.
- `src/microlink.c`, `src/ml_coord.c`, `include/microlink.h`, GitLab #26:
  `microlink_rotate_node_key()` replaces the node key in NVS and keeps the old
  public key (`wg_old_pub`), which the register request then carries as
  `OldNodeKey`, as tailscaled sends its persisted `OldPrivateNodeKey`.
  Upstream never regenerated the node key (its own comment said to factory
  reset); `../tailnet` rotates when `microlink_needs_new_key()` turns true.
- `src/ml_coord.c`, GitLab #28: `RegisterResponse.AuthURL` with no auth key
  in the request sets `node_key_rejected` and fails the register (it was
  logged and treated as registered; a node deleted from the tailnet gets
  this).
- `src/ml_coord.c`, `include/microlink.h`, review F-B7 and F-B5:
  `RegisterResponse.Error` sets `register_refused`, a netmap that overflows
  the buffers sets `netmap_too_large`; either puts the reconnect backoff at
  `ML_CTRL_HOLD_OFF_MS` (5 min) instead of at most 16 s, and both have
  getters for the supervisor.
- `src/microlink.c`, review F-B8: the key writes and commit are checked and
  `microlink_keys_saved()` reports the result; `identity_persistent` keeps
  its meaning.
- `src/ml_coord.c`, review F-B6: `coord_recv`/`noise_recv` return
  `COORD_RX_IDLE` (nothing read) or `COORD_RX_FAIL` (closed, error, or out of
  step) and no caller reads `errno` any more; `recv() == 0` is a close.
- `src/ml_coord.c`, review F-B9: the handshake body buffer is at least 512
  bytes (1-2 byte overflow when the 101 response carried 1 or 2 body bytes).
- `src/ml_coord.c`, review F-B10: GOAWAY, or RST_STREAM on the map stream,
  ends the long-poll session and reconnects.
- `src/ml_coord.c`, review F-B11: `do_noise_handshake` clears the node-key
  challenge first.
- `src/ml_coord.c`, review F-B12: a netmap peer with no `Key` is dropped;
  dotted-quad addresses and endpoint ports are range-checked.
- Host pins: `tools/test/test_tailnet_keys.py`.
- `src/ml_stun.c`, review G-B12: the IPv6 parser checks `attr_len >= 4`
  before it reads an attribute's family byte (`tools/test/test_stun.py`).
- `include/microlink_internal.h`, `src/ml_derp.c`, `src/ml_net_io.c`,
  `src/ml_wg_mgr.c`, review G-B8: the four queue-drop warnings are limited
  by time (`ml_drop_log_due`, one per 10 s), not one per 32 drops.
- `src/ml_wg_mgr.c`, `src/nacl_box.c`, review G-B10: an all-zero DISCO key
  matches no peer, and `nacl_box_beforenm` refuses an all-zero shared secret
  (its two one-shot callers check it).
- `src/ml_derp.c`, review G-B11: a full relay TX queue drops the oldest
  packet that is not a WireGuard handshake (handshakes are queued at the
  front, which is where room used to be made).
- `src/ml_wg_mgr.c`, review G-B13: `wg_on_tcpip()` returns whether it ran
  the call, and a received datagram whose hand-off failed is freed.
- Pins: `tools/test/test_tailnet_dataplane.py`, `test_tailnet_fixes.py`.

Security fixes (advisory GHSA-jxrh-hg4w-g4rm, fixed upstream in
v2.2.0; the WireGuard side is in `../wireguard_lwip/VENDORED.md`). Ported
as targeted changes, not a rebase onto v2.2.0: this copy carries the
patches above, and upstream restructured the files they touch.

- `src/ml_wg_mgr.c`, `src/ml_peer_nvs.c`, from upstream 2e567c6 and
  3f2216a: endpoints are filtered with `wg_endpoint_ip4_is_valid_underlay()`
  (`../wireguard_lwip/src/wg_endpoint.h`) wherever they enter: netmap and
  patch endpoint lists (`ml_copy_endpoints`), the NVS peer cache on load,
  CallMeMaybe entries, and the endpoint given to `wireguardif_add_peer`.
  `disco_udp_sendto` and `wg_udp_output_cb` refuse a non-underlay
  destination, so no direct DISCO or WireGuard packet can be sent into the
  tunnel whatever the source of the address. `ml_wg_mgr_trigger_handshake`
  connects only if `wireguardif_update_endpoint` took the address.
  `tools/test/test_wg_endpoint.py`.
- `src/ml_wg_mgr.c`, `src/ml_disco_match.h` (new), from upstream 2cfc1fb:
  every DISCO ping destination (best path, each netmap endpoint, each
  CallMeMaybe entry, the relay) gets its own ping and txid, and the probe
  records `dest_ip`/`dest_port`. A PONG counts only if it is sealed by the
  pinged peer's disco key, and a direct one only if it came from exactly
  the pinged address (`ml_disco_pong_path`); a relayed PONG is liveness
  only. One txid used to go to every address, so a captured PONG re-sent
  from another address made that address the peer's WireGuard endpoint.
  `MAX_PENDING_PROBES` is 64 (was 32) and the table is `EXT_RAM_BSS_ATTR`
  (PSRAM on Rev B). `tools/test/test_disco_pong.py`.
- `src/ml_wire_checks.h` (new, header-only), `src/ml_noise.c`,
  `src/ml_coord.c`, `src/ml_derp.c`, from upstream dd29cf6, af3a58e,
  d6493c0 and debc839: Noise msg2 must be exactly 48 bytes (checked in
  `ml_noise_read_msg2` and, before anything is read or allocated for it, in
  `do_noise_handshake`); a longer one was decrypted into a NULL buffer
  before the server was authenticated. The control and DERP HTTP upgrades
  check the status line (`ml_http_status_is_101`) instead of `strstr("101")`.
  A Noise frame over 16 KB is refused before allocation
  (`ml_noise_ct_len_ok`). `coord_recv_ex()` finishes a started read within
  30 s of progress and 90 s in all and stops on shutdown, and `noise_recv`
  reads the payload as one such read (it was 300 retries, each up to a whole
  SO_RCVTIMEO). A DERP RecvPacket of 32 bytes or less is dropped (it was
  dispatched as a packet from an all-zero key). `tools/test/test_ml_wire.py`;
  `test_tailnet_keys.py` follows `coord_recv_ex`.
- `include/ml_filter.h`, `src/ml_filter.c` (new), `src/ml_coord.c`,
  `src/ml_wg_mgr.c`, `src/microlink.c`, `include/microlink_internal.h`,
  `Kconfig`, `CMakeLists.txt`, from upstream 82f05fd and a379946 (the
  tailnet packet filter): the tailnet policy (`MapResponse.PacketFilter`
  and `PacketFilters`, read from the full map and every streamed update) is
  enforced on inbound tunnel packets, as tailscaled's wgengine/filter does:
  new TCP connections, UDP and ICMP need a matching rule (SrcIPs, DstPorts
  IP and ports, IPProto; CapGrant-only rules grant nothing); non-SYN TCP,
  replies to UDP flows the board started, ICMP echo replies and errors
  always pass. Nothing new passes before the first policy (tailscaled
  starts allow-nothing); a set that cannot be applied allows nothing. In
  every mode a packet must be addressed to the board's tailnet IP. Kconfig
  `ML_PACKET_FILTER` (Enforce, the default; Log only; Off) and
  `ML_FILTER_MAX_ENTRIES` (4096). Adapted: IPv4 only, no advertised routes
  (upstream's `ml_routes` API is not here), rule tables and the flow table
  in PSRAM, a non-null legacy `PacketFilter` replaces every named set (as
  tailscaled's map session does; upstream replaced only "base"), the filter
  is reset in `microlink_init` rather than `microlink_destroy` (an instance
  freed late by `microlink_reap()` must not wipe its successor's policy),
  and a drop is logged at INFO at most once a minute. ICMP (any protocol without ports) needs a rule for
  all ports, as tailscaled's `matchProtoAndIPsOnlyIfAllPorts`; upstream let
  a single-port rule admit pings.
  `tools/test/test_ml_filter.py` runs it against ESP-IDF's cJSON.
- `src/ml_wg_mgr.c`, from upstream 944055b: a full peer record whose NodeID
  is in the table under another node key is a key rotation; the old key's
  slot, WireGuard peer and NVS entry are dropped (`remove_peer_idx`, split
  out of `remove_peer`) before the new key is installed. The rotated-out key
  stayed installed and accepted until a full netmap swept it.
  `tools/test/test_tailnet_peer_table.py`.
- Upstream commits looked at and not taken: bf528f8 (`ml_udp.c` close-drain;
  that socket layer is not used here), 1cb8d1c (host tests only; the cases
  it covers, replay before roaming and cookie replies, are in
  `test_wg_replay.py`), the rest of a379946 (MapResponse JSON offset
  detection, interactive login, NAPT for subnet routes: not security fixes,
  or for features this copy does not have).

