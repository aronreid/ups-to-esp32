/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The IPv6 "is this client on our network" rule. Pure, so
 * tools/test/test_ip6_peer.py compiles it on the host.
 *
 * With IPv4 the router's NAT keeps the internet out. A SLAAC address is
 * globally routable, and whether the internet can reach it is down to the
 * router's IPv6 firewall, which some routers leave open. NUT reads and (with
 * no login set) the page need no password, so over IPv6 the board answers
 * only clients that are plainly local:
 *   - link-local, fe80::/10;
 *   - unique local, fc00::/7 (never routed on the internet);
 *   - an address in the same /64 as one of the board's own;
 *   - loopback, and IPv4 (v4-mapped) which this rule does not cover.
 * Anything else, a routed IPv6 client on another subnet included, is refused,
 * unless the owner has turned on "answer IPv6 from other networks" (GitHub
 * #20, netmgr_ip6_peer_allowed below).
 */
#ifndef UPSA_NETMGR_IP6_H
#define UPSA_NETMGR_IP6_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* peer: the client's 16 bytes. mine: n of the board's addresses, 16 bytes
 * each, as they sit in memory (network order). */
static inline bool netmgr_ip6_peer_local(const uint8_t peer[16], const void *mine, int n)
{
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    static const uint8_t loop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (memcmp(peer, mapped, sizeof(mapped)) == 0) return true;      /* IPv4 */
    if (memcmp(peer, loop, sizeof(loop)) == 0) return true;
    if (peer[0] == 0xfe && (peer[1] & 0xc0) == 0x80) return true;   /* fe80::/10 */
    if ((peer[0] & 0xfe) == 0xfc) return true;                      /* fc00::/7 */
    const uint8_t *m = (const uint8_t *)mine;
    for (int i = 0; i < n; i++, m += 16) {
        /* Our link-local's prefix is fe80::/64: already answered above. */
        if (memcmp(m, peer, 8) == 0) return true;
    }
    return false;
}

/* A routed client (GitHub #20): a global unicast address, 2000::/3, that is
 * not on this network. Answered only when the owner has turned on "answer
 * IPv6 from other networks" AND the board has a login (netmgr_ip6.c):
 * a SLAAC address can be reachable from the internet if the router lets
 * IPv6 in, and a board with no login could otherwise be claimed from there. */
static inline bool netmgr_ip6_peer_routed(const uint8_t peer[16])
{
    return (peer[0] & 0xe0) == 0x20;
}

static inline bool netmgr_ip6_peer_allowed(const uint8_t peer[16], const void *mine, int n,
                                           bool routed_on)
{
    if (netmgr_ip6_peer_local(peer, mine, n)) return true;
    return routed_on && netmgr_ip6_peer_routed(peer);
}

#endif /* UPSA_NETMGR_IP6_H */
