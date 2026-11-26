/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How other tasks talk to ups_task, and how they tell it is still alive.
 * Pure logic, pulled out of ups_hid.c so tools/test/test_ups_task_link.py can
 * run it on the host without ESP-IDF.
 */
#ifndef UPSA_UPS_TASK_LINK_H
#define UPSA_UPS_TASK_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* ---- Answers carry the number of the request they answer ---------------
 *
 * Requests to ups_task share one answer queue. A caller that gave up on its
 * request (a 5 s INSTCMD timeout while a poll sat on a parked control
 * transfer) leaves that request in the queue, and ups_task still answers it
 * later. Before answers were numbered, the NEXT caller took that late answer
 * as its own: a quiet request "succeeded" while USB was still live, and the
 * outage log or an update then wrote flash with the interrupt pipe running,
 * the hcd_dwc.c crash ups_hid.h records for v0.14.4. Now every request gets a
 * number, ups_task echoes it, and a caller skips answers that are not its own. */
typedef struct {
    uint32_t seq;
    int      res;       /* an esp_err_t */
} cmd_answer_t;

/* The next request number. Never 0, so a zeroed answer matches nothing. */
static inline uint32_t cmd_seq_next(uint32_t *counter)
{
    if (++*counter == 0) ++*counter;
    return *counter;
}

/* Wait for the answer to request `seq`. `recv` takes the next answer, or
 * returns false once the caller's deadline has passed; answers to earlier
 * requests are thrown away on the way. True, with *res set, if ours came. */
static inline bool cmd_await(uint32_t seq, bool (*recv)(void *ctx, cmd_answer_t *a),
                             void *ctx, int *res)
{
    cmd_answer_t a;
    while (recv(ctx, &a)) {
        if (a.seq == seq) {
            *res = a.res;
            return true;
        }
    }
    return false;
}

/* ---- ups_task's heartbeat ----------------------------------------------
 *
 * ups_task stamps the time, in ms, at the top of every loop. A task stuck for
 * good (usb_host_endpoint_halt() waiting forever inside ESP-IDF, see
 * QUIET_HUNG_MS in ups_hid.c) stops stamping, and main.c's supervisor
 * restarts the board. 32 bits so another task reads it in one go; the
 * difference is taken mod 2^32, so the 49-day wrap does not matter, and a
 * stamp newer than `now_ms` (the reader read the clock first) is not hung.
 * `last_ms` 0: the task has not started, or never will (USB off). */
static inline uint32_t task_beat_stamp(uint32_t now_ms)
{
    return now_ms ? now_ms : 1;
}

static inline bool task_beat_hung(uint32_t last_ms, uint32_t now_ms, uint32_t limit_ms)
{
    return last_ms != 0 && (int32_t)(now_ms - last_ms) > (int32_t)limit_ms;
}

/* ---- Per-report read health --------------------------------------------
 *
 * A report that stops answering must not keep serving what it said last.
 * Each polled report keeps a count of reads missed in a row and whether it has
 * answered at all since the UPS attached. */
typedef struct {
    uint8_t miss;       /* reads missed in a row */
    uint8_t seen;       /* answered at least once since the UPS attached */
} poll_health_t;

/* Misses in a row after which a report's values are dropped. Five polls is
 * ~10 s, the patience FAILURES_BEFORE_RESET gives a UPS that answers nothing;
 * a single missed read is common and should not blank a value. */
#define POLL_HEALTH_STALE_MISSES 5

static inline void poll_health_note(poll_health_t *h, bool answered)
{
    if (answered) {
        h->seen = 1;
        h->miss = 0;
    } else if (h->miss < UINT8_MAX) {
        h->miss++;
    }
}

/* It has answered before and did not this time: what it carries is unknown
 * for this poll. One that has never answered is not "lost": the UPS may not
 * implement it, and it never contributed anything to keep. */
static inline bool poll_health_lost(const poll_health_t *h)
{
    return h->seen && h->miss > 0;
}

/* Missed long enough that its last values must not be shown any more. */
static inline bool poll_health_stale(const poll_health_t *h)
{
    return h->miss >= POLL_HEALTH_STALE_MISSES;
}

#endif /* UPSA_UPS_TASK_LINK_H */
