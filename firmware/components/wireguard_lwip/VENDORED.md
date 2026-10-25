# Vendored: wireguard-lwip

BSD 3-clause, Daniel Hope (www.floorsense.nz); see LICENSE. Upstream project
notes are kept as README.upstream.md.

- Original: https://github.com/smartalock/wireguard-lwip, as modified for
  MicroLink (magicsock/DERP output hooks).
- Taken from https://github.com/Csontikka/esphome-tailscale,
  `microlink/components/microlink/components/wireguard_lwip/`, repo commit
  `a3067dd4dffaed39c9eda108ea4a4758ddb48b73`.

MicroLink nests this component inside its own tree; here it exists once, as
a top-level component, and `../microlink` requires it by name.

## Copied

`src/*.c`, `src/*.h`, `src/crypto/refc/`, `CMakeLists.txt`, `LICENSE`.
Left out: `example/` and `src/crypto/cortex/` (ARM Cortex-M assembly; this
build uses the portable reference crypto, `WIREGUARD_CRYPTO_REFC`).

## Local changes

Each is marked with a `local patch` comment in the source. To update: re-copy
the same files from a newer esphome-tailscale commit and reapply these.

- `src/wireguardif.h`: `WIREGUARDIF_MTU` 1280, upstream 1420. Tailscale's own
  tun runs at 1280, and at 1420 inbound TCP from Linux peers stalls (upstream
  MicroLink issue #34).
- `CMakeLists.txt`: `SRCS` emptied unless `CONFIG_UPSA_TAILSCALE`; the
  compile definition and the per-file `-Wno-stringop-overread` (upstream's,
  for a GCC false positive in x25519.c) are applied only when it is built,
  since an empty component is an INTERFACE library.
- `src/wireguard.c`: the handshake initiation rate limit compares `now - last`.
  Upstream compared `last - now`, so the limit never engaged.
- `src/wireguardif.c`, `WG_ON_TCPIP()`: every entry point
  (`wireguardif_output`, `_network_rx`, `_periodic`, `_connect*`,
  `_add_peer`, `_remove_peer`, `_update_endpoint`, `_add_allowed_ip`,
  `_peer_is_up`, `_disconnect`, `_shutdown`, `_free`, `_inject_packet`)
  first calls `wireguardif_check_tcpip_thread()`, which logs once, at ERROR,
  if it is not on the lwIP TCPIP thread, without aborting. The device and the
  raw lwIP calls in this file are owned by that thread (see
  `../microlink/VENDORED.md`, "TCPIP-thread ownership"); the check is how a
  regression shows up on a board. Two comments that said the periodic work
  ran off the TCPIP thread on purpose are corrected. No behaviour change on
  the right thread.
- `src/wireguardif.c`, `wireguardif_periodic`: a comment above
  `netif_set_link_down` explains the link-down routing behaviour (with no
  peer session the netif is link-down and lwIP stops routing 100.64/10 through
  it, so the board cannot open a session itself). No code change.
