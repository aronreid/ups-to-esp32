/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * When the fleet check-in runs, and when a Modbus capture rides along with
 * it. Kept free of ESP-IDF so tools/test/test_fleet_sched.py can run a
 * simulated day of a server that is down, failing or refusing, against the
 * same code fleet_task runs: a bug here once had a board with an unreachable
 * server retry every 10 s instead of backing off.
 */
#ifndef UPSA_FLEET_SCHED_H
#define UPSA_FLEET_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A few times a day is enough to know a board is alive, and is nothing to a
 * free database. The server can change it in its reply, within these bounds. */
#define FLEET_DEFAULT_INTERVAL_S  (6 * 60 * 60)
#define FLEET_MIN_INTERVAL_S      (60 * 60)
#define FLEET_MAX_INTERVAL_S      (24 * 60 * 60)
/* Failures back off from five minutes to the normal interval. */
#define FLEET_RETRY_S             300

/* After a check-in: `got` > 0 is the interval the server asked for, <= 0 a
 * failure of any kind. Returns the seconds until the next one. */
static inline int32_t fleet_after_checkin(int got, int32_t *interval, int *fails)
{
    if (got > 0) {
        *interval = got < FLEET_MIN_INTERVAL_S ? FLEET_MIN_INTERVAL_S
                  : got > FLEET_MAX_INTERVAL_S ? FLEET_MAX_INTERVAL_S : got;
        *fails = 0;
        return *interval;
    }
    int32_t back = FLEET_RETRY_S << (*fails < 6 ? *fails : 6);
    (*fails)++;
    return back < *interval ? back : *interval;
}

/* Is an unasked Modbus capture owed for the UPS with descriptor `hash`?
 * `consent`: check-in on, and stats ticked or a test board. `mb_state` is
 * ups_hid_modbus_state(). `tried` is the hash one was last tried for (once per
 * boot per UPS, whatever the outcome). The first time one is owed the next
 * check-in is brought forward to within 10 s, and only that first time
 * (`forwarded`), so a server that cannot be reached is not asked every 10 s. */
static inline bool fleet_mb_owed(bool consent, int mb_state, const char *hash,
                                 const char *tried, char forwarded[17], int32_t *due)
{
    if (!consent || mb_state != 2 || !hash[0] || !strcmp(hash, tried)) return false;
    if (strcmp(hash, forwarded) != 0) {
        strncpy(forwarded, hash, 16);
        forwarded[16] = '\0';
        if (*due > 10) *due = 10;
    }
    return true;
}

#endif /* UPSA_FLEET_SCHED_H */
