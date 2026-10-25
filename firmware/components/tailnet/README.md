# tailnet: optional Tailscale on the board (Rev B)

Joins the board to a tailnet with MicroLink (`../microlink`, vendored; see its
`VENDORED.md`), so NUT on 3493 and the web UI answer on a 100.x address from
anywhere on the tailnet, with no subnet router and no host computer.

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
Rev B releases include Tailscale.

## sdkconfig.defaults.tailscale

Each option is checked against MicroLink's source. From its example sdkconfig,
deliberately not taken:

- `FREERTOS_HZ=1000`. Global, and only one compiled-in delay needs it; that
  one is patched instead (`../microlink/VENDORED.md`, DERP read loop delay).
- AMPDU off, bigger Wi-Fi buffers, 240 MHz. Latency tuning for video; NUT
  traffic is a few hundred bytes every few seconds.
- `ESP_TASK_WDT_TIMEOUT_S=30`. A watchdog is what reboots a wedged closet
  device; the 5 s default stays. If a DERP or control TLS handshake trips it,
  that is the first thing to look at.
- `MBEDTLS_HKDF_C`. MicroLink's HKDF is its own, over BLAKE2s.
- `LWIP_IP_FORWARD`. Only for subnet routing or an exit node; not offered.

`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is on for every Rev B build, in
`sdkconfig.defaults.revb`, because internal RAM runs out without it.

## Settings: the auth key

Never in the image or sdkconfig: one image is published for every Rev B board.
MicroLink's own Kconfig credential fields exist but are read only by its config
HTTP server, which is compiled out.

The web UI shows a Tailscale card only on firmware that has it. It posts to
`POST /api/tailscale`:

    {"enabled": true, "key": "tskey-auth-..."}   store key, switch on
    {"enabled": false}                           switch off, keep key
    {"enabled": false, "forget": true}           erase key and tailnet identity

Stored in NVS namespace `upsa` (keys `ts_key`, `ts_on`, `ts_reg`), next to
Wi-Fi, so the BOOT-five-times factory reset clears it too. MicroLink keeps its
machine, node and DISCO keys in its own namespaces (`microlink`, `ml_peers`);
both "forget" and the factory reset erase those with `microlink_factory_reset()`.
`/api/status` reports `tailscale.{state, enabled, key_set, registered, ip, peers}`,
never the key. Changes apply without a restart.

**The auth key is kept only until the board has registered.** Once the board is
on the tailnet with an address, `ts_key` is erased from flash and the copy
MicroLink was started with is wiped from RAM; `ts_reg` records that the node is
registered, and the node key MicroLink holds is the identity from then on
(`key_set` turns false, `registered` true). The key is asked for again only when
the control plane stops accepting the node key (`microlink_needs_new_key()`: a
`NodeKeyExpired` registration reply, or the node marked expired in the netmap):
the state then reads "no key" and a new one registers the board afresh.

Like every other route that changes something, `/api/tailscale` is behind the
board's login (`may_write` in `webui.c`); the page and `/api/status` need it
too once one exists. Joining the board to a tailnet makes the page and NUT
reachable from the tailnet as well, so tailnet ACLs are what limit who can
reach ports 80, 443 and 3493 there. NUT reads stay open, as on the LAN.

Use a reusable or a pre-approved key, and disable key expiry for this machine
in the admin console: once registered the board keeps its node key across
reboots and does not keep the auth key. If the node key expires the board needs
a new auth key entered on the page, because it no longer holds the old one.

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
- LEDs, `board_status_t` and the OLED do not show Tailscale state; it is only
  on the web page and in `/api/status`.

## Not supported

- Subnet routing and exit nodes (`LWIP_IP_FORWARD` is off).
- Headscale or any control plane other than Tailscale's: only `tskey-` keys
  are accepted, and a DERP relay with a private CA fails certificate
  verification (see below).
- Connections the board itself opens over the tailnet (see link-down above).

## Home DERP region

The board's home relay is MicroLink's default, region 4 (Frankfurt,
`ML_DERP_REGION`), for every board, wherever it is. Peers that cannot reach
the board directly relay through Frankfurt, which adds latency outside Europe;
a direct path, the usual case on a home network, does not use the relay at
all. Kept on purpose: it is the only region with a record of working with
this code (upstream tried region 26 and the session broke; see the comment at
`ML_DERP_REGION`).

The latency probe that could pick a region (`ml_netcheck`) is not run:
`tailnet.c` leaves `netcheck_override_enabled` off, and the probe runs only
with the override on, stopping as soon as every region has answered. Turning
the override on (with `netcheck_override_threshold_ms` as hysteresis) is the
way to a nearer relay.

## DERP relay TLS

The relay's certificate is checked against the IDF certificate bundle and its
host name. DERP nodes in Tailscale's map are `*.tailscale.com` with public
certificates; a relay with a private CA (a self-hosted Headscale DERP) fails
and keeps retrying on the DERP backoff (5 s doubling to 60 s).

Certificate dates are not checked: `CONFIG_MBEDTLS_HAVE_TIME_DATE` is off in
this build, as for every other TLS client on the board (control plane, OTA,
fleet). So a DERP connect right after GOT_IP, before SNTP has set the clock,
verifies normally and there is no wait for the clock. If that option is ever
turned on, every one of those connections fails until SNTP answers (and for
good on a network that blocks NTP), so it needs a clock gate first.

## Local changes

The vendored stack carries local patches, each marked `local patch` in the
source. `../microlink/VENDORED.md` and `../wireguard_lwip/VENDORED.md` list
the ones in their files; the following are in this component or span both.

- `tailnet.c`: the event group and mutex are static, and `tailnet_get()` and
  `tailnet_configure()` are NULL-safe before `tailnet_start()`.
- `microlink/src/ml_wg_mgr.c` and `wireguard_lwip/src/wireguardif.c`,
  "TCPIP-thread ownership": the TCPIP thread is the single owner of `struct
  wireguard_device` and of every raw lwIP call. This build has lwIP core
  locking off, so `udp_sendto`, `netif_set_*`, `netif_remove` and the
  `netif_list` splice are legal only there. wg_mgr hands packets and commands
  over with synchronous `tcpip_api_call()` messages (`wg_on_tcpip()`):
  nothing to allocate, nothing lost, FIFO with lwIP's own posts. Teardown is
  two such calls (detach, then free) so no queued packet outlives its netif.
  The handshake crypto runs on the TCPIP thread; the comment at the top of
  `ml_wg_mgr.c` says why and lists which thread may touch each field.
  `WG_ON_TCPIP()` in `wireguardif.c` checks every entry point and logs the
  first violation once, as an error, without aborting.
- `sdkconfig.defaults.tailscale`: `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096`
  (IDF default 3072), one more KB of internal RAM, for the handshakes and
  the vendored WARN logs that run on that thread.
