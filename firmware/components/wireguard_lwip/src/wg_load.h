/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * When is the WireGuard responder "under load"? Pure, so
 * tools/test/test_wg_load.py can compile it on the host.
 *
 * WireGuard's defence against handshake floods is the cookie: under load a
 * responder answers an initiation that has a valid mac1 but no valid mac2
 * with a cookie reply, and does the X25519 work only for a sender that proves
 * it can receive at its address. wireguard_is_under_load() used to return
 * false for ever, so every mac1-valid initiation (mac1 is derived from this
 * board's public key, which any tailnet member holds) cost a full handshake's
 * public-key work on the lwIP thread.
 *
 * Under load = more than WG_LOAD_MAX_INITS mac1-valid initiations inside the
 * last WG_LOAD_WINDOW_US, or internal heap below WG_LOAD_MIN_HEAP.
 */
#ifndef UPSA_WG_LOAD_H
#define UPSA_WG_LOAD_H

#include <stdbool.h>
#include <stdint.h>

#define WG_LOAD_WINDOW_US   1000000
#define WG_LOAD_MAX_INITS   4
#define WG_LOAD_RING        8       /* more than WG_LOAD_MAX_INITS */
#define WG_LOAD_MIN_HEAP    (40 * 1024)

/* ts: the times (us) of recent mac1-valid initiations, 0 = empty slot. */
static inline bool wg_under_load_window(const int64_t *ts, int n, int64_t now)
{
    int recent = 0;
    for (int i = 0; i < n; i++) {
        if (ts[i] > 0 && now - ts[i] >= 0 && now - ts[i] < WG_LOAD_WINDOW_US) recent++;
    }
    return recent > WG_LOAD_MAX_INITS;
}

#endif
