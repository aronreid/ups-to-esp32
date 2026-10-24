/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure test behind netmgr_local_is_ap(), kept in a header so it can be
 * compiled on the host (tools/test/test_ap_addr.py).
 *
 * A connection arrived on the setup/rescue SoftAP exactly when the address it
 * was made TO (the socket's local address) is the AP's own address. Judging
 * by the peer would be a guess, and a subnet match would wrongly catch a home
 * network that happens to use the same address range as the setup network.
 * Both values are in network byte order; an AP address of 0 means there is no AP, and nothing matches it.
 */
#ifndef UPSA_NETMGR_AP_H
#define UPSA_NETMGR_AP_H

#include <stdbool.h>
#include <stdint.h>

static inline bool netmgr_addr_is_ap(uint32_t local_addr, uint32_t ap_addr)
{
    return ap_addr != 0 && local_addr == ap_addr;
}

#endif
