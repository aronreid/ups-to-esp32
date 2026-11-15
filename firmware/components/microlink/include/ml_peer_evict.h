/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * local patch (GitHub #18): which peer gives up its slot when the table is
 * full. Pure, so tools/test/test_tailnet_peer_table.py compiles it on the host.
 *
 * Upstream evicts only for the configured priority peer, so on a tailnet with
 * more peers than slots the first ML_MAX_PEERS in netmap order held the table
 * for good and a later device (a new phone) could never reach the board.
 *
 * Who may take a slot from whom, for a peer not already in the table:
 *   - a peer that has CONTACTED the board (a DISCO packet from a peer that
 *     was refused for lack of room and is waiting, ml_wg_mgr.c) may take the
 *     slot of one that is offline, else one that is idle;
 *   - a peer that is merely in the netmap and online may take only an
 *     offline peer's slot. Not an idle online one: on a tailnet of 30 idle
 *     devices that only rotated the table in netmap order, every netmap, and
 *     the device that wanted the board still waited its turn;
 *   - a peer that is offline takes nothing.
 * Never evicted: the priority peer, an exit node, a peer with a live
 * WireGuard session, or one with activity (sent to, DISCO pong, or added) in
 * the last ML_EVICT_IDLE_MS. Within the allowed set, offline goes before
 * online and the oldest activity first. The idle floor bounds churn: a slot
 * turns over at most once per ML_EVICT_IDLE_MS.
 */
#ifndef ML_PEER_EVICT_H
#define ML_PEER_EVICT_H

#include <stdbool.h>
#include <stdint.h>

#define ML_EVICT_IDLE_MS (10ULL * 60 * 1000)

typedef struct {
    bool     active;
    bool     protect;        /* priority peer or exit node */
    bool     session_up;     /* a valid WireGuard keypair right now */
    bool     online;         /* control's liveness flag */
    uint64_t last_activity_ms; /* max of sent, DISCO pong, added */
} ml_evict_view_t;

enum { ML_EVICT_IN_OFFLINE = 0, ML_EVICT_IN_ONLINE = 1, ML_EVICT_IN_WANTED = 2 };

/* Index to evict, or -1 for none. incoming: ML_EVICT_IN_*. */
static inline int ml_peer_pick_evict(const ml_evict_view_t *v, int n,
                                     uint64_t now_ms, int incoming)
{
    if (incoming != ML_EVICT_IN_ONLINE && incoming != ML_EVICT_IN_WANTED) return -1;
    int best = -1;
    bool best_offline = false;
    uint64_t best_age = 0;
    for (int i = 0; i < n; i++) {
        const ml_evict_view_t *p = &v[i];
        if (!p->active || p->protect || p->session_up) continue;
        uint64_t age = now_ms > p->last_activity_ms ? now_ms - p->last_activity_ms : 0;
        if (age < ML_EVICT_IDLE_MS) continue;
        bool off = !p->online;
        if (!off && incoming != ML_EVICT_IN_WANTED) continue;
        /* Offline beats online; then older beats newer. */
        if (best < 0 || (off && !best_offline) ||
            (off == best_offline && age > best_age)) {
            best = i; best_offline = off; best_age = age;
        }
    }
    return best;
}

#endif /* ML_PEER_EVICT_H */
