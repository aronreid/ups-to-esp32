# Vendored: wireguard-lwip

BSD 3-clause, Daniel Hope (www.floorsense.nz); see LICENSE. Upstream project
notes are kept as README.upstream.md.

- Original: https://github.com/smartalock/wireguard-lwip, as modified for
  MicroLink (magicsock/DERP output hooks).
- Taken from https://github.com/Csontikka/esphome-tailscale,
  `microlink/components/microlink/components/wireguard_lwip/`, repo commit
  `a3067dd4dffaed39c9eda108ea4a4758ddb48b73` (2026-09-10).

MicroLink nests this component inside its own tree; here it exists once, as
a top-level component, and `../microlink` requires it by name.

## Copied

`src/*.c`, `src/*.h`, `src/crypto/refc/`, `CMakeLists.txt`, `LICENSE`.
Left out: `example/` and `src/crypto/cortex/` (ARM Cortex-M assembly; this
build uses the portable reference crypto, `WIREGUARD_CRYPTO_REFC`).

## Local patches

1. `src/wireguardif.h`: `WIREGUARDIF_MTU` 1280, was 1420. Tailscale's own tun
   runs at 1280 and at 1420 inbound TCP from Linux peers stalls (upstream
   MicroLink issue #34).
2. `CMakeLists.txt`: `SRCS` emptied unless `CONFIG_UPSA_TAILSCALE`; the
   compile definition and the per-file `-Wno-stringop-overread` (upstream's,
   for a GCC false positive in x25519.c) are applied only when it is built,
   since an empty component is an INTERFACE library.
