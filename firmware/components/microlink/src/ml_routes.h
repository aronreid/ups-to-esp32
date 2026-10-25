/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which prefixes a tailnet peer may claim on this board. Header-only so
 * tools/test/test_ml_routes.py can compile it on the host.
 *
 * Every prefix in a peer's AllowedIPs used to become a WireGuard allowed
 * source range, with no opt-in. "0.0.0.0/1" plus "128.0.0.0/1" (which slip
 * past the exit-node string check) let a peer source packets from any IPv4
 * address, the LAN's and 127.0.0.1 included. The board never dials out over
 * the tunnel, so it takes no routes at all: only addresses inside the
 * tailnet's own 100.64.0.0/10 are recognised, and those are the peer's own
 * address, which is installed separately.
 */
#ifndef UPSA_ML_ROUTES_H
#define UPSA_ML_ROUTES_H

#include <stdbool.h>
#include <stdint.h>

/* net in host byte order, plen 0..32. True only for a prefix wholly inside
 * 100.64.0.0/10 (so a /9 that merely contains it is not). */
static inline bool ml_prefix_is_tailnet(uint32_t net, int plen)
{
    return plen >= 10 && plen <= 32 && (net & 0xFFC00000UL) == 0x64400000UL;
}

#endif
