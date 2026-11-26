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
- `src/wireguardif.c`, `src/wg_timers.h`, GitLab #29: `wireguardif_periodic`
  destroys `prev_keypair` and `next_keypair` once older than
  `REJECT_AFTER_TIME` (upstream aged out only `curr_keypair`); a keepalive is
  due only with a current keypair, and `last_tx` is stamped after every
  keepalive attempt, sent or not; a keepalive (`q == NULL`) no longer re-arms
  the lazy handshake in `wireguardif_output_to_peer`, only real data does.
  Upstream, a peer that had left was sent an initiation every 5 s for ever.
  `wireguardif_tmr` (armed only by the socket-bind init path, not used in
  magicsock mode) now calls `wireguardif_periodic` instead of carrying a
  second copy of its rules. The rules are pure in `wg_timers.h`;
  `tools/test/test_wg_timers.py`.
- `src/wireguardif.c`, GitLab #31: `[WG_OUT_FAIL]` has its own limiter: the
  first failure, then one line a minute with the count of those not logged.
  `src/wireguard-platform-esp32.c`: the `[TAI64N]` `printf` is gone.
  `tools/test/test_wg_log_level.py`.
- `src/wireguardif.c`, review G-B6: decrypted inner IPv6 packets are dropped
  (there was no allowed-source check for them; the tunnel has no IPv6
  address).
- `src/wireguard.c`, review G-B15: `wireguard_generate_unique_index` stops at
  the first collision (it compared only the last peer).

Security fixes (upstream advisories CamM2325/microlink
GHSA-jxrh-hg4w-g4rm, smartalock/wireguard-lwip GHSA-7grw-r329-8g8c,
fugo101/wireguard-lwip GHSA-mqvf-8p8p-wjqg, GHSA-xr9h-pc9x-m684,
GHSA-335q-jvfv-r737; ported one by one, not by re-copying, because of the
patches above):

- `src/wg_endpoint.h` (new, header-only), `src/wireguardif.c`, from upstream
  MicroLink 2e567c6 and 43b6074: a peer endpoint must be an IPv4 underlay
  address (`wg_endpoint_usable()`): not 0/8, 127/8, the tailnet's
  100.64/10, 169.254/16 or 224/3 (the IPv6 rules are there for the
  callers that need them). `update_peer_addr` ignores any other address,
  `wireguardif_update_endpoint` returns `ERR_ARG` for one,
  `wireguardif_add_peer` makes such a configured endpoint relay-only, and
  `wireguardif_network_rx` drops a datagram whose source is one (0.0.0.0,
  the relay, still passes). A 100.x endpoint, which any tailnet peer could
  set by sending WireGuard through the tunnel, made every later packet loop
  back into the netif and recurse on the TCPIP thread until the board
  rebooted. `tools/test/test_wg_endpoint.py`.
- `src/wireguardif.c`, cookie reply (smartalock f2d67f8, whitepaper 5.4.7):
  a valid cookie reply only stores the cookie. It moved the endpoint, and
  anyone who knows a peer's public key and saw one initiation can forge one.
- `src/wireguardif.c`, handshake initiation: the response goes to the
  initiation's source (or DERP), but that source no longer becomes the
  endpoint; the first authenticated transport packet sets it. A captured
  initiation replayed after a restart (`greatest_timestamp` lives in RAM)
  moved the endpoint. The timestamp is still not persisted (smartalock's
  `remote_initiation_accepted` API is not ported): a replay after a restart
  can still start a responder handshake, which goes nowhere.
- `src/wg_replay.h` (new, header-only), `src/wireguard.c`,
  `src/wireguard.h`: the replay window measures counter distance in 64 bits
  and is 64 packets wide (`replay_bitmap` is `uint64_t`).
  `tools/test/test_wg_replay.py`.
- `src/wireguardif.c`, `wireguardif_process_data_message` (smartalock
  aefa97a): Reject-After-Messages is checked against the received counter
  and only drops the packet (it compared the sending counter and destroyed
  the keypair). `keypair_update()` returns the slot the keypair lives in
  after next moves to current; the rekey check after it read the wiped next
  slot.
- `src/wireguardif.c`, `src/wireguard.c`, `src/wireguard.h` (smartalock
  8db46df): `peer->last_session_millis` is set by every new session, and
  `should_reset_peer` (whitepaper 6.3, 3 x Reject-After-Time with no new
  session) keys off it; it tested `curr_keypair`, destroyed at 1 x, so the
  reset never ran. The reset also wipes the handshake state.
- `src/wireguard.c`, `wireguard_start_session`: `add_new_keypair` takes a
  pointer and the stack copy of the session keys is wiped.
- `src/wireguardif.c`, `peer_owns_source` (fugo101 GHSA-335q-jvfv-r737):
  an inner packet's source must route back to the sending peer, the longest
  allowed-IP match over every peer; it only had to fall in one of the
  sender's own ranges, so the exit node (0.0.0.0/0) could send as any
  other peer's tailnet address.
- Checked and already in this copy: the replay check before any state change
  and for keepalives (smartalock f204508), short plaintext (95fa4b1), the
  initiation rate limit (89444f7), unique index (cbc7826), inner IPv6 dropped.
  Not ported: fugo101 GHSA-mqvf-8p8p-wjqg (transmit falls back to a less
  specific peer without a session): `peer_lookup_by_allowed_ip` here prefers
  a keypair only between equally specific matches, and the board installs
  only each peer's /32 and the exit node's 0/0, so a /32 always wins.
- `src/wireguard.h`, `src/wireguardif.c`, `src/wireguardif.h`, from
  upstream MicroLink 82f05fd: `wireguardif_set_ip_filter()` installs an
  inbound filter (asked for every decrypted IPv4 packet before
  `netif->input`) and an outbound observer (the first 28 bytes of each
  packet in `wireguardif_output`). MicroLink uses it for the tailnet ACLs
  (`../microlink/include/ml_filter.h`).

