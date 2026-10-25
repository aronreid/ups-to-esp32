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
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

static inline bool netmgr_addr_is_ap(uint32_t local_addr, uint32_t ap_addr)
{
    return ap_addr != 0 && local_addr == ap_addr;
}


/* The IPv4 address (network byte order) in a socket address, or 0.
 *
 * The web server listens on a dual-stack IPv6 socket, so getsockname() and
 * getpeername() answer with a sockaddr_in6 holding "::ffff:a.b.c.d" for an
 * IPv4 client. Read as a sockaddr_in, those 16 bytes give the wrong four
 * (sin6_flowinfo, which is 0), and "is this the AP's address" never matched.
 * Here: AF_INET gives its address, a v4-mapped AF_INET6 gives its last four
 * bytes, anything else (a native IPv6 address, a short length) gives 0. */
static inline uint32_t netmgr_sockaddr_v4(const struct sockaddr_storage *ss, socklen_t len)
{
    if (!ss || len < (socklen_t)sizeof(sa_family_t)) return 0;
    if (ss->ss_family == AF_INET) {
        if (len < (socklen_t)sizeof(struct sockaddr_in)) return 0;
        return ((const struct sockaddr_in *)ss)->sin_addr.s_addr;
    }
#if !defined(ESP_PLATFORM) || LWIP_IPV6
    if (ss->ss_family == AF_INET6) {
        if (len < (socklen_t)sizeof(struct sockaddr_in6)) return 0;
        const uint8_t *a = (const uint8_t *)&((const struct sockaddr_in6 *)ss)->sin6_addr;
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(a, mapped, sizeof(mapped)) != 0) return 0;
        uint32_t v4;
        memcpy(&v4, a + 12, sizeof(v4));
        return v4;
    }
#endif
    return 0;
}

#endif
