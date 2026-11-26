/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which DISCO PONGs prove a direct path. Header-only so
 * tools/test/test_disco_pong.py can compile it on the host.
 *
 * local patch (2026-11-25), from upstream MicroLink 2cfc1fb. A PONG's
 * outer UDP source is not authenticated, and one ping (one txid) used to go
 * to every address of a peer, so an on-path attacker could re-send a captured
 * PONG from its own address and become the peer's direct WireGuard endpoint.
 * Each destination now gets its own ping, recorded with the probe, and a
 * direct PONG counts only from exactly that address. A PONG through the
 * relay is liveness only. The PONG must also be sealed by the disco key of
 * the peer that was pinged (checked by the caller).
 */
#ifndef UPSA_ML_DISCO_MATCH_H
#define UPSA_ML_DISCO_MATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ML_PONG_REJECT = 0,   /* direct, but not from the address pinged */
    ML_PONG_DIRECT,       /* direct, from exactly the pinged address */
    ML_PONG_RELAY,        /* through DERP: the peer is alive, no path learnt */
} ml_pong_path_t;

/* dest_ip/dest_port: where the matched ping was sent (host order), 0/0 when
 * it went through the relay. src_*: where the PONG came from. */
static inline ml_pong_path_t ml_disco_pong_path(bool via_derp, uint32_t src_ip, uint16_t src_port,
                                                uint32_t dest_ip, uint16_t dest_port)
{
    if (via_derp) return ML_PONG_RELAY;
    if (dest_ip != 0 && dest_port != 0 && src_ip == dest_ip && src_port == dest_port)
        return ML_PONG_DIRECT;
    return ML_PONG_REJECT;
}

#endif
