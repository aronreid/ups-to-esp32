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

Like every other settings route on this board, `/api/tailscale` has no login:
whoever can reach the page can already change its Wi-Fi or switch the UPS
load off. Joining the board to a tailnet extends that reach to the tailnet, so
tailnet ACLs are what limit who can reach port 80 and 3493 there.

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
- LEDs, `board_status_t` and the OLED are unchanged; Tailscale state is only on
  the web page and in `/api/status`.

## Unverified until a Rev B board exists

- That it registers, connects, and serves NUT and HTTP over the tunnel at all.
- Internal RAM at run time: MicroLink's five tasks take ~46 KB of internal
  stack, plus `tailnet`'s 6 KB (a guess; `CONFIG_UPSA_MEM_DIAG` measures it).
- That TLS handshakes stay inside the 5 s task watchdog at 160 MHz.
- MTU 1280 against a real Linux peer, and reconnection after an AP reboot.
- NVS headroom: 24 KB partition, peer cache capped at 16 peers (1.5 KB).
- `microlink_destroy()` leaks the instance (logged) if a task will not stop in
  15 s; repeated, that would drain PSRAM.
EOF