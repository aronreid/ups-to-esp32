# tailnet: optional Tailscale on the board (Rev B)

Joins the board to a tailnet with MicroLink (`../microlink`, vendored; see its
`VENDORED.md`), so NUT on 3493 and the web UI answer on a 100.x address from
anywhere on the tailnet, with no subnet router and no host computer.

Unverified on hardware: no Rev B board exists yet (2026-09-26). Everything
below is what the code does and what the build shows, not what a board did.

## Building

Off by default and Rev B only (`UPSA_TAILSCALE` depends on `UPSA_BOARD_REV_B`
and `SPIRAM`). With it off, `tailnet`, `microlink` and `wireguard_lwip` compile
no sources, the web UI has no Tailscale route or JSON, and the map file has no
MicroLink symbols.

    idf.py -B build-revb-ts -D SDKCONFIG=build-revb-ts/sdkconfig \
      -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.revb;sdkconfig.defaults.tailscale" \
      -D UPSA_IMAGE_NAME=ups-adaptor-revb build

The image name is the plain Rev B one on purpose: OTA refuses an image whose
name differs from the running one, and a Tailscale build is the same board.
Whether release.yml should ship Tailscale in the Rev B asset is not decided
here.

Measured 2026-09-26, IDF 5.4: 1,231,888 bytes, 39% of the 3 MB slot, against
1,101,120 for plain Rev B. MicroLink is about 63 KB of code, WireGuard 18 KB,
the rest is mbedTLS ChaCha/Poly and lwIP growth. Static internal RAM grows by
about 2.7 KB.

## sdkconfig.defaults.tailscale

Each option was checked against MicroLink's source. From its example sdkconfig,
deliberately NOT taken:

- `FREERTOS_HZ=1000`. Global, and only one compiled-in delay needed it; that
  one is patched instead (`../microlink/VENDORED.md`, patch 3).
- AMPDU off, bigger Wi-Fi buffers, 240 MHz. Latency tuning for video; NUT
  traffic is a few hundred bytes every few seconds.
- `ESP_TASK_WDT_TIMEOUT_S=30`. A watchdog is what reboots a wedged closet
  device; do not loosen it on a guess. If a DERP/control TLS handshake on real
  hardware trips the 5 s watchdog, that is the first thing to look at.
- (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` was on this list until 2026-10-09; it is on now, for every Rev B build, in `sdkconfig.defaults.revb`, because measured internal RAM ran out without it.)
- `MBEDTLS_HKDF_C`. MicroLink's HKDF is its own, over BLAKE2s.
- `LWIP_IP_FORWARD`. Only for subnet routing or an exit node; not offered.

## Settings: the auth key

Never in the image or sdkconfig: one image is published for every Rev B board.
MicroLink's own Kconfig credential fields exist but are read only by its config
HTTP server, which is compiled out.

The web UI shows a Tailscale card only on firmware that has it. It posts to
`POST /api/tailscale`:

    {"enabled": true, "key": "tskey-auth-..."}   store key, switch on
    {"enabled": false}                           switch off, keep key
    {"enabled": false, "forget": true}           erase key and tailnet identity

Stored in NVS namespace `upsa` (keys `ts_key`, `ts_on`), next to Wi-Fi, so the
BOOT-five-times factory reset clears it too. MicroLink keeps its machine, node
and DISCO keys in its own namespaces (`microlink`, `ml_peers`); "forget" erases
those with `microlink_factory_reset()`, the factory reset does not.
`/api/status` reports `tailscale.{state, enabled, key_set, ip, peers}`, never
the key. Changes apply without a restart.

Like every other route that changes something, `/api/tailscale` is behind the
board's login (`may_write` in `webui.c`); the page and `/api/status` need it
too once one exists. Joining the board to a tailnet makes the page and NUT
reachable from the tailnet as well, so tailnet ACLs are what limit who can
reach ports 80, 443 and 3493 there. NUT reads stay open, as on the LAN.

Use a reusable or a pre-approved key, and disable key expiry for this machine
in the admin console: once registered the board keeps its node key across
reboots, but if the node key expires it has to register again with the auth
key it holds.

## Behaviour

- Starts when the station has an address (`netmgr_state()` is the source of
  truth), named `ups-esp32-XXXX` like the setup AP and mDNS.
- Wi-Fi drops: left to MicroLink, whose coordinator retries with backoff. When
  the address returns, `microlink_rebind()` forces the control and DERP links
  to reconnect at once.
- Not connected for 10 minutes with Wi-Fi up: torn down and rebuilt, doubling
  to at most an hour so a bad key does not churn the board.
- Never restarts the board. netmgr's own 15-minute offline reboot is unchanged.
- The web UI can call `tailnet_get()`/`tailnet_configure()` before
  `tailnet_start()` has run (the web server starts first): the status reads
  "off" and a settings change is refused ("could not save") until it has.
- No peer holding a session: the WireGuard netif goes link-down and lwIP
  stops routing 100.64/10 through it, so traffic the board itself starts
  toward the tailnet cannot open a session. Everything today is a peer
  connecting in, which brings the link up. See the comment above
  `netif_set_link_down` in `wireguardif_periodic` before adding anything that
  dials out over the tailnet (MQTT to a tailnet broker, say).

## Home DERP region

The board's home relay is MicroLink's default, region 4 (Frankfurt,
`ML_DERP_REGION`), for every board, wherever it is. Peers that cannot reach
the board directly relay through Frankfurt, which adds latency outside Europe;
a direct path, the usual case on a home network, does not use the relay at
all. Kept on purpose: it is the only region with a record of working with
this code (upstream tried region 26 and the session broke; see the comment at
`ML_DERP_REGION`), and nothing in this tree has measured a better one.

The latency probe that could pick a region (`ml_netcheck`) is not run:
`tailnet.c` leaves `netcheck_override_enabled` off, and with it off the probe
used to run anyway and its result was thrown away, stalling the control task
up to 25 s per DERP map. It now runs only with the override on, and stops as
soon as every region has answered. Turning the override on (with
`netcheck_override_threshold_ms` as hysteresis) is the way to a nearer relay;
it wants a Pi Rig test of relayed sessions first.

## DERP relay TLS

The relay's certificate is checked against the IDF certificate bundle and its
host name (it was not checked at all). DERP nodes in Tailscale's map are
`*.tailscale.com` with public certificates; a relay with a private CA (a
self-hosted Headscale DERP) would fail and keep retrying on the DERP backoff
(5 s doubling to 60 s). This firmware only takes `tskey-` keys, i.e.
Tailscale's own control plane.

Certificate dates are not checked: `CONFIG_MBEDTLS_HAVE_TIME_DATE` is off in
this build, as for every other TLS client on the board (control plane, OTA,
fleet). So a DERP connect right after GOT_IP, before SNTP has set the clock,
verifies normally and there is no wait for the clock. If that option is ever
turned on, every one of those connections fails until SNTP answers (and for
good on a network that blocks NTP), so it needs a clock gate first;
`tools/test/test_tailnet_fixes.py` fails if it is turned on.

## Local patches (2026-10-17 review, items T1 to T11)

Each is marked in the source with a `local patch` comment. Add them back if
MicroLink or wireguard-lwip is ever re-vendored (see `../microlink/VENDORED.md`
and `../wireguard_lwip/VENDORED.md` for the earlier ones).

- `microlink/src/ml_derp.c`, `microlink.c`, `microlink_internal.h` (T1):
  DERP reader/writer teardown. The reader holds `derp.rx_lock` while it
  touches the TLS context or socket, re-checks `connected` on every retry of a
  payload read and leaves; the writer frees the context only while holding
  the lock (`derp_teardown`, 10 s bound). If the bound runs out nothing is
  freed: the writer falls into the normal reconnect backoff and
  `ml_derp_connect()` retries the teardown before it reinitialises anything.
  A PONG is queued to the writer instead of written from the reader. The
  comment that called concurrent read and write on one context mbedTLS's
  documented model is corrected: it is not; what is relied on is stated.
- `ml_coord.c`, `ml_netcheck.c` (T4): netcheck only when the override is on;
  early exit once every probed region has answered.
- `ml_wg_mgr.c`, `ml_derp.c` (T5): per-packet and every-10-s log lines moved to
  DEBUG (WG UDP TX, WG RX, WG INIT, direct probe, WG_SNAP, DISCO counts, DERP
  HEARTBEAT and status); "DERP output cb: not connected" once per 32 drops.
  This firmware logs at INFO on a 115200-baud console, from the TCPIP thread.
- `ml_peer_nvs.c`, `ml_wg_mgr.c` (T6): the peer cache is marked dirty and
  written once per batch of peer updates (`ml_peer_nvs_flush`), and a peer
  re-sent unchanged is not a write at all. Was one ~1.9 KB NVS write per peer.
- `ml_derp.c` (T7): `MBEDTLS_SSL_VERIFY_REQUIRED` with `esp_crt_bundle_attach`.
- `microlink.c` (T8): the TAI64N base is stored on every boot with a flag
  saying whether it may run ahead of real time, and picked by
  `ml_tai_base_pick()`: wall clock minus uptime, never below the stored base
  (a day above it if that one ran ahead); without a clock, stored + one day.
- `ml_net_io.c`, `ml_derp.c` (T9): received datagram copies in PSRAM
  (`ml_psram_malloc`), like the queue payloads.
- `wireguard_lwip/src/wireguardif.c` (T10): comment only, link-down routing.
- `tailnet.c` (T11, our code): static event group and mutex, NULL-safe
  `tailnet_get`/`tailnet_configure` before `tailnet_start`.
- `wireguard_lwip/src/wireguard.c`: initiation rate limit compared
  `last - now`, so it never engaged; now `now - last` (upstream bug).
- `ml_derp.c`: ServerInfo length off the wire capped at 4 KB, and a short
  read of it fails the connect instead of desynchronising the stream.
- LEDs, `board_status_t` and the OLED are unchanged; Tailscale state is only on
  the web page and in `/api/status`.

## Unverified until a Rev B board exists

- That it registers, connects, and serves NUT and HTTP over the tunnel at all.
- Internal RAM at run time: MicroLink's five tasks take ~46 KB of internal
  stack, plus `tailnet`'s 6 KB (a guess; `CONFIG_UPSA_MEM_DIAG` measures it).
- That TLS handshakes stay inside the 5 s task watchdog at 160 MHz.
- MTU 1280 against a real Linux peer, and reconnection after an AP reboot.
- NVS headroom: 24 KB partition, peer cache capped at 16 peers (118 bytes
  each, 1.9 KB), written once per batch of peer updates.
- `microlink_destroy()` leaks the instance (logged) if a task will not stop in
  15 s; repeated, that would drain PSRAM.

## Local patches to the vendored stack (2026-10-17)

Both vendored components carry small patches, each marked `ups-driver local
patch` in the source and listed in its `VENDORED.md`. The ones made for the
2026-10-17 bug hunt (items T2 and T3, Rev B only):

- `microlink/src/ml_wg_mgr.c`, "TCPIP-thread ownership": the TCPIP thread is
  the single owner of `struct wireguard_device` and of every raw lwIP call.
  This build has lwIP core locking off, so `udp_sendto`, `netif_set_*`,
  `netif_remove` and the `netif_list` splice were only ever legal there, and
  the device (peers, keypairs, the ChaCha20-Poly1305 nonce counter) was
  shared between the TCPIP thread's TX and the wg_mgr task's RX, rekey and
  keepalives with no lock. wg_mgr now hands packets and commands over with
  synchronous `tcpip_api_call()` messages (`wg_on_tcpip()`): nothing to
  allocate, nothing lost, FIFO with lwIP's own posts. Teardown is two such
  calls (detach, then free) so no queued packet outlives its netif. The
  handshake crypto runs on the TCPIP thread; the long comment at the top of
  the file says why, and lists which thread may touch each field.
- `wireguard_lwip/src/wireguardif.c`, `WG_ON_TCPIP()`: every entry point
  checks it is on the TCPIP thread and logs the first violation once, as an
  error, without aborting. Two comments that described the old split were
  corrected.
- `sdkconfig.defaults.tailscale`: `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096`
  (IDF default 3072), one more KB of internal RAM, for the handshakes and
  the vendored WARN logs that now run on that thread.

Pinned by `tools/test/test_wg_tcpip_owner.py` (a raw lwIP call or a device
write outside the TCPIP-thread functions fails the test) and modelled by
`tools/test/test_wg_owner_model.py` (nonce, keypair install and teardown
ordering, exhaustively interleaved on the host). Not yet run on a board:
the Pi Rig plan is in the bug-hunt handoff.
