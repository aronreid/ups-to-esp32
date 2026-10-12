# Vendored: MicroLink (Tailscale client for ESP-IDF)

MIT, Cameron Malone; see LICENSE and x25519-license.txt.

- Upstream: https://github.com/CamM2325/microlink
- Taken from the copy maintained in https://github.com/Csontikka/esphome-tailscale
  (`microlink/components/microlink/`), repo commit
  `a3067dd4dffaed39c9eda108ea4a4758ddb48b73` (2026-09-10); last commit touching
  that directory `4bfb3a7`. That copy is based on upstream's dev line `058b374`
  and carries fixes upstream has not merged, most importantly the lwIP threading
  fix (upstream issues #15/#17, PR #28): the WireGuard netif's `input` is
  `tcpip_input` (`src/ml_wg_mgr.c`, `netif->input = tcpip_input`), so decrypted
  packets reach TCP on the TCPIP thread rather than from `ml_wg_mgr`. It also
  fixes the stop/destroy leaks and use-after-free that a reconnect used to hit.

Only built with `CONFIG_UPSA_TAILSCALE`; the glue is `../tailnet`.

## Copied

`src/`, `include/`, `Kconfig`, `CMakeLists.txt`, `LICENSE`, `x25519-license.txt`.

Left out: examples, docs, `ml_config_override.h` (ESPHome-only string defaults),
and the nested `components/wireguard_lwip`, which lives once, at
`../wireguard_lwip`.

## Local patches

1. `Kconfig`: the whole menu is inside `if UPSA_TAILSCALE`, so a build without
   Tailscale has no `CONFIG_ML_*` symbols; `ML_ENABLE_CONFIG_HTTPD` is
   `depends on n`. That server binds port 80, which the web UI owns, and is an
   unauthenticated page that rewrites Wi-Fi and the auth key.
   `../tailnet/tailnet.c` also `#error`s if it is ever on.
2. `CMakeLists.txt`: `SRCS` is emptied unless `CONFIG_UPSA_TAILSCALE`, so Rev A
   and plain Rev B compile none of it (REQUIRES stays unconditional: CONFIG_
   values are unknown when IDF first expands requirements). Dropped the
   PlatformIO-only `esp-tls` include hack; native IDF gets it from REQUIRES.
3. `src/ml_derp.c`, DERP payload read loop: `vTaskDelay(pdMS_TO_TICKS(10))`,
   was 5. At this firmware's 100 Hz tick, 5 ms is 0 ticks, and the loop would
   spin at priority 5 for up to 5 s and starve IDLE0 into the task watchdog.
   Upstream is only exercised at ESPHome's 1000 Hz. The only sub-tick delay in
   the sources that are compiled here (cellular/AT-socket code is compiled out).

To update: re-copy the same files from a newer esphome-tailscale commit and
reapply the three patches above; each is marked "ups-driver local patch".

## upsa patch: four task stacks in PSRAM (2026-10-09)

`ml_net_io`, `ml_derp_tx`, `ml_derp_rx` and `ml_coord` are created by
`ml_task_create_psram()` (`src/microlink.c`) with their stacks in PSRAM,
falling back to internal RAM, and end with `ML_TASK_EXIT()` (see the next
patch). On the first Rev B board, three UPSes and
Tailscale left no 12 KB internal block and `ml_coord` failed to start. None of
the four writes flash (a task with a PSRAM stack must not: the cache is off
while flash is written); `ml_wg_mgr` saves peers to NVS and keeps an internal
stack.

## upsa patch: those four tasks never delete themselves (2026-10-11)

`ML_TASK_EXIT()` (`include/microlink_internal.h`) was
`vTaskDeleteWithCaps(NULL)`. A task created with caps cannot free its own
stack, so for `NULL` ESP-IDF 5.5 creates a temporary helper task to delete it
and calls `abort()` if that helper cannot be created
(`components/freertos/esp_additions/idf_additions.c`). On a board with a few
hundred bytes of internal RAM left (seen on the rig: 416 B), a Wi-Fi drop or a
Tailscale restart that stops MicroLink could reboot the board.

Now each of the four tasks calls `ml_task_exiting()`, which sets its bit in
`ml->tasks_parked` and then drops `tasks_alive`, and parks in
`ML_TASK_EXIT()` (`vTaskSuspend(NULL)` forever). `microlink_stop()` waits for
`tasks_alive` to reach zero as before, then `ml_delete_parked()` calls
`vTaskDeleteWithCaps(handle)` from the stopping task for each task whose bit is
set. Deleting another task takes the path that allocates nothing: suspend,
wait until it is off both cores, `vTaskDelete()`, free the stack and TCB. A
task that never signed off (the stop wait timed out) is not deleted; it is
leaked with the instance, as `stop_incomplete` already did. Every exit path
goes through here: the tasks only exit on `ML_EVT_SHUTDOWN_REQUEST`, which
only `microlink_stop()` sets, and a failed `microlink_start()` is followed by
`microlink_destroy()`, which stops first (`../tailnet/tailnet.c`).
`ml_wg_mgr` is created without caps and still ends with `vTaskDelete(NULL)`,
which needs no helper task.

## upsa patch: `ml_wg_mgr` stack 6 KB (2026-10-11)

`ML_TASK_WG_MGR_STACK` is 6 KB, was 8 KB. Its observed peak is about 4.3 KB
(the comment above the stack sizes in `include/microlink_internal.h`). It
stays in internal RAM because it writes peers to NVS.

## Local edits

Three comments gave example routes in 192.168.x ranges; changed to the
documentation ranges 203.0.113.0/24 and 198.51.100.0/24 so the public-tree
scan (tools/publish.sh) does not read them as someone's network. No code change.
