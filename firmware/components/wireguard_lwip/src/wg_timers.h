/* SPDX-License-Identifier: BSD-3-Clause (local patch to vendored wireguard-lwip,
 * GitLab #29 / #31)
 *
 * The keepalive and keypair-ageing decisions of wireguardif_periodic(), pure,
 * so tools/test/test_wg_timers.py compiles them on the host. Times are
 * wireguard_sys_now() milliseconds (32-bit, compared by modular subtraction,
 * as wireguard_expired() does); intervals are seconds.
 *
 * What went wrong before: only curr_keypair was ever aged out, so after one
 * rekey prev_keypair stayed valid for ever once a peer left. The keepalive
 * test accepted curr OR prev, a keepalive with no usable keypair failed
 * without updating last_tx (so it was retried every 400 ms tick, each failure
 * a WARN line on the lwIP thread), and that failure re-armed the lazy
 * handshake, undoing the 18-attempt cap: a peer that had left was sent a
 * handshake initiation every 5 s for ever, about 40 ms of lwIP thread each.
 */
#ifndef UPSA_WG_TIMERS_H
#define UPSA_WG_TIMERS_H

#include <stdbool.h>
#include <stdint.h>

static inline bool wg_tmr_expired(uint32_t since_ms, uint32_t now_ms, uint32_t secs)
{
    return (uint32_t)(now_ms - since_ms) >= secs * 1000u;
}

/* prev and next keypairs are destroyed once they are older than
 * REJECT_AFTER_TIME: nothing may be sent or accepted on them after that. */
static inline bool wg_tmr_keypair_stale(bool valid, uint32_t keypair_ms, uint32_t now_ms,
                                        uint32_t reject_after_s)
{
    return valid && wg_tmr_expired(keypair_ms, now_ms, reject_after_s);
}

/* A keepalive only when the current keypair exists (an old prev keypair is
 * not a session), and at most one attempt per interval: the caller stamps
 * last_tx after every attempt, sent or not. */
static inline bool wg_tmr_keepalive_due(uint16_t interval_s, bool curr_valid,
                                        uint32_t last_tx_ms, uint32_t now_ms)
{
    return interval_s > 0 && curr_valid && wg_tmr_expired(last_tx_ms, now_ms, interval_s);
}

/* Output with no usable keypair arms the lazy handshake (and re-arms it after
 * the attempts ran out) only for real data from lwIP, never for a keepalive:
 * a peer nobody is talking to is left alone once its attempts are spent. */
static inline bool wg_tmr_rearm_handshake(bool is_data, bool active, uint8_t attempts,
                                          uint8_t max_attempts)
{
    return is_data && (!active || attempts >= max_attempts);
}

#endif
