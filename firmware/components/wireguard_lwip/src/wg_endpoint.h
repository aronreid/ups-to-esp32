/* ups-driver local patch (2026-11-25), from upstream MicroLink 2e567c6 and
 * 43b6074 (wg_endpoint_is_valid_underlay). Header-only so
 * tools/test/test_wg_endpoint.py can compile it on the host.
 *
 * A WireGuard or DISCO peer endpoint must be a real underlay address. The
 * magicsock socket is bound to 0.0.0.0, so a tailnet peer can send a valid
 * WireGuard message through the tunnel to the board's 100.x address; taken
 * as the peer's endpoint, every later packet to that peer is routed back
 * into the WireGuard netif, which encrypts it again, and the TCPIP thread
 * recurses until its stack overflows (reboot). A CallMeMaybe, a netmap or
 * the NVS peer cache can hand over the same kind of address. Refused:
 * 0.0.0.0/8, loopback 127/8, the tailnet's 100.64.0.0/10, link-local
 * 169.254/16, multicast, reserved and broadcast (224/3), and for IPv6 the
 * tailnet fd7a:115c:a1e0::/48, multicast, link-local fe80::/10, ::/96
 * (unspecified, loopback, IPv4-compatible) and ::ffff:0:0/96 (IPv4-mapped,
 * which would carry an IPv4 address past the IPv4 checks).
 */
#ifndef UPSA_WG_ENDPOINT_H
#define UPSA_WG_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

/* IPv4 address in host byte order. */
static inline bool wg_endpoint_ip4_is_valid_underlay(uint32_t a)
{
    if ((a >> 24) == 0 || (a >> 24) == 127) return false;     /* 0/8, loopback */
    if ((a & 0xFFC00000u) == 0x64400000u) return false;       /* 100.64/10 tailnet */
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return false;       /* 169.254/16 */
    if ((a >> 28) >= 0xEu) return false;                       /* 224/3 incl. broadcast */
    return true;
}

/* IPv6 address as four 32-bit words in host byte order (w[0] is the top). */
static inline bool wg_endpoint_ip6_is_valid_underlay(const uint32_t w[4])
{
    if (w[0] == 0xfd7a115cu && (w[1] >> 16) == 0xa1e0u) return false;  /* tailnet /48 */
    if ((w[0] >> 24) == 0xffu) return false;                           /* multicast */
    if ((w[0] & 0xffc00000u) == 0xfe800000u) return false;             /* fe80::/10 */
    if (w[0] == 0 && w[1] == 0 && (w[2] == 0 || w[2] == 0x0000ffffu)) return false;
    return true;
}

#endif
