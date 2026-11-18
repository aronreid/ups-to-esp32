// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Two decisions netmgr.c makes from timers and events, as pure functions so
 * the host can run them (tools/test/test_netmgr_decide.py). No ESP-IDF here. */
#include <stdbool.h>
#include <stdint.h>

/* ---- The fixed address's gateway proof ------------------------------------
 *
 * A fixed address is only kept if the gateway answers from it: a mistyped one
 * would otherwise leave the board unreachable, so it falls back to DHCP until
 * restart. The proof used to be one round of pings on EVERY reconnect, and one
 * failed round was final. A board that reconnected while its router was still
 * starting (access points up first, or the router restarted on its own) lost
 * its address for good, and the NUT clients pointed at it lost the board.
 *
 * Now: an address proven once this session is not put on trial again, and an
 * unproven one gets NETMGR_PROOF_TRIES rounds, NETMGR_PROOF_GAP_US apart,
 * before the fallback. The board answers on the fixed address meanwhile. */
#define NETMGR_PROOF_TRIES   6
#define NETMGR_PROOF_GAP_US  ((int64_t)20 * 1000000)

typedef enum {
    NETMGR_PROOF_KEEP = 0,      /* proven: keep the address, stop asking   */
    NETMGR_PROOF_AGAIN,         /* no answer yet: ask again after the gap  */
    NETMGR_PROOF_FALL_BACK,     /* never answered: DHCP until restart      */
} netmgr_proof_next_t;

/* After a round. *fails counts rounds without an answer since the last one
 * with; the caller keeps it for the session. */
static inline netmgr_proof_next_t netmgr_proof_next(bool answered, int *fails)
{
    if (answered) { *fails = 0; return NETMGR_PROOF_KEEP; }
    if (++*fails >= NETMGR_PROOF_TRIES) return NETMGR_PROOF_FALL_BACK;
    return NETMGR_PROOF_AGAIN;
}

/* On getting an address: is there anything to prove? */
static inline bool netmgr_proof_needed(bool is_static, bool fell_back, bool proven)
{
    return is_static && !fell_back && !proven;
}

/* ---- The rescue setup window ----------------------------------------------
 *
 * Rescue setup stays open for a window, and the window is pushed out while
 * someone is on the setup network. With no limit, a phone that had remembered
 * the open setup network and rejoined it by itself held the board in setup,
 * off its own Wi-Fi, for as long as the phone stayed in range. The window can
 * now be pushed out only up to `cap_us` after setup opened; a join under way
 * is never cut short. */
#define NETMGR_RESCUE_CAP_US  ((int64_t)30 * 60 * 1000000)

/* One tick of the rescue timer. Returns true when setup should close and the
 * stored network be tried again; otherwise *until_us may have moved out. */
static inline bool netmgr_rescue_tick(int64_t now_us, int64_t opened_us, int64_t *until_us,
                                      bool someone, bool joining,
                                      int64_t window_us, int64_t cap_us)
{
    if (joining) { *until_us = now_us + window_us; return false; }
    int64_t cap = opened_us + cap_us;
    if (someone && now_us < cap) {
        int64_t out = now_us + window_us;
        *until_us = out < cap ? out : cap;
        return false;
    }
    return now_us >= *until_us;
}
