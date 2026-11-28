/* ups-driver local patch (2026-11-25). Header-only so
 * tools/test/test_wg_replay.py can compile it on the host.
 *
 * The anti-replay window for received transport data (RFC 2401 appendix C,
 * as wireguard-lwip had it), with two fixes from the upstream advisories:
 * the distance between counters is 64-bit (it was a uint32_t, so a counter
 * 2^32 ahead looked like a small step and a packet far ahead or behind could
 * land in the window), and the window is 64 packets (was 32), which the
 * bitmap now holds. Counters are WireGuard's, starting at 0; internally they
 * are shifted by one so that 0 means "nothing received yet".
 */
#ifndef UPSA_WG_REPLAY_H
#define UPSA_WG_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define WG_REPLAY_WINDOW 64u

/* Returns true, and records seq, if seq has not been seen and is not older
 * than the window. Call only for an authenticated packet. */
static inline bool wg_replay_check(uint64_t *bitmap, uint64_t *highest, uint64_t seq)
{
    seq++;
    if (seq == 0) return false;                         /* counter wrapped */
    if (seq > *highest) {
        uint64_t diff = seq - *highest;
        *bitmap = (diff < WG_REPLAY_WINDOW) ? ((*bitmap << diff) | 1u) : 1u;
        *highest = seq;
        return true;
    }
    uint64_t diff = *highest - seq;
    if (diff >= WG_REPLAY_WINDOW) return false;         /* too old */
    uint64_t bit = (uint64_t)1 << diff;
    if (*bitmap & bit) return false;                     /* seen */
    *bitmap |= bit;
    return true;
}

#endif
