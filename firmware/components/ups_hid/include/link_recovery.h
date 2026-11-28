/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure decisions for a UPS that answers for a few minutes and then goes
 * quiet: when to read it lightly, and how hard to reset its link. Pulled out
 * of ups_hid.c so tools/test/test_link_recovery.py can compile and run them
 * on the host, like status_hold.h.
 *
 * Light polling. Every two seconds ups_task reads every planned feature
 * report plus a few of NUT's table reports: well over ten control transfers.
 * NUT's usbhid-ups reads only its status reports at that rate and the rest
 * every 30 s. Some UPSes stop answering under the heavier load; an APC
 * Back-UPS BX1600MI answered for 4 to 6 minutes after each replug and then
 * went quiet for good (GitHub issue #5). Read lightly, a UPS gets its status
 * reports every poll, as before, so a power cut is seen as quickly, and
 * everything else on one poll in LIGHT_POLL_FULL_EVERY.
 *
 * A UPS is read lightly when its model is one known to need it, or when it
 * has already gone quiet after answering this boot (learned, kept until
 * restart, for that USB ID only).
 *
 * Link reset. The first automatic recovery is a VBUS cycle; a self-powered
 * UPS does not disconnect through that, so it is closed and enumerated again
 * in place, which re-reads descriptors but never resets its USB interface.
 * From the second attempt in a row, a UPS still connected after the VBUS
 * cycle has the host port powered off and on instead: a real disconnect and
 * a bus reset, the nearest thing to pulling the cable.
 *
 * Lost to the port cycle. A UPS normally enumerates again a second or two
 * after the port powers on. One that does not is not waited for: with no
 * device there is nothing to poll, so nothing counts as failing and no
 * further recovery ever runs. An APC Back-UPS BX1600MI came back from 9 port
 * cycles and stayed away after the 10th, read as "not detected" until the
 * board was restarted by hand (GitHub issue #5). The board's own restart
 * brings the USB host up from scratch, which is what brought that UPS back
 * every time, so after PORT_CYCLE_BACK_US without it the board restarts.
 * That cannot loop: a restart is only ever asked for by a port cycle, and a
 * port cycle needs a UPS that attached and answered.
 */
#ifndef UPSA_LINK_RECOVERY_H
#define UPSA_LINK_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LIGHT_POLL_FULL_EVERY 5     /* with 2 s polls: everything every 10 s */

enum { LIGHT_POLL_NO = 0, LIGHT_POLL_MODEL = 1, LIGHT_POLL_LEARNED = 2 };

/* Models known to go quiet under the full polling rate. APC's VID only:
 * other vendors' "BX" names mean nothing here. */
static inline bool light_poll_model(uint16_t vid, uint16_t pid, const char *model)
{
    (void)pid;
    if (vid != 0x051D || !model) return false;
    if (strncmp(model, "Back-UPS ", 9) == 0) model += 9;
    return model[0] == 'B' && model[1] == 'X' && model[2] >= '0' && model[2] <= '9';
}

/* Why this UPS should be read lightly, LIGHT_POLL_NO if it should not.
 * `learned` is true when a UPS with this USB ID went quiet after answering
 * earlier this boot. */
static inline int light_poll_choose(uint16_t vid, uint16_t pid, const char *model,
                                    bool learned, uint16_t learned_vid, uint16_t learned_pid)
{
    if (light_poll_model(vid, pid, model)) return LIGHT_POLL_MODEL;
    if (learned && vid == learned_vid && pid == learned_pid) return LIGHT_POLL_LEARNED;
    return LIGHT_POLL_NO;
}

/* Whether poll number `tick` (counted from 0 per attachment) reads every
 * report. The first poll after an attach always does. */
static inline bool light_poll_full(uint32_t tick)
{
    return tick % LIGHT_POLL_FULL_EVERY == 0;
}

/* Recovery attempt `attempt` (1 for the first since the UPS last answered):
 * whether a UPS that stayed connected through the VBUS cycle gets the host
 * port power-cycled rather than re-enumerated in place. */
static inline bool recover_use_port_cycle(uint32_t attempt)
{
    return attempt >= 2;
}

/* How long after the board's own port power cycle the UPS has to enumerate
 * again before the board restarts. Far beyond the second or two it takes. */
#define PORT_CYCLE_BACK_US (30LL * 1000 * 1000)

/* Whether a UPS taken off by the board's own port cycle is lost to it.
 * `since_us` is when the wait began (0: no port cycle is being waited on),
 * `have_dev` whether a UPS has enumerated since. */
static inline bool port_cycle_lost(int64_t since_us, int64_t now_us, bool have_dev)
{
    return since_us != 0 && !have_dev && now_us - since_us > PORT_CYCLE_BACK_US;
}

/* A report the UPS never answers (GitLab #83). Some UPSes stop answering one
 * control request while still answering others and sending their interrupt
 * reports. ups_hid.c gives up on such a request (cancels it) after
 * CTRL_ABORT_AFTER_US, before the polls missed meanwhile add up to the
 * "stopped answering" count that starts the recovery ladder, so a UPS that
 * answers the next request carries on with no VBUS or port cycle. If the same
 * report then sticks again, every few polls would be a cancel, readings would
 * flap and the ladder would still run in the end; so a feature report
 * cancelled CTRL_CANCELS_BEFORE_DROP times in a row (no good read of it in
 * between) is no longer asked for until the UPS enumerates again. In a row,
 * not in one attachment: a UPS attached for months would otherwise lose a
 * reading to two unrelated stalls weeks apart. Only a report that carries no
 * status and no control is ever dropped (ups_hid.c decides): those are what
 * a power cut and a shutdown depend on, and a UPS deaf to them stays on the
 * recovery ladder as before. */
#define CTRL_CANCELS_BEFORE_DROP 2

/* The report ID a control request asks for, if it is GET_REPORT(Feature)
 * on an interface (bmRequestType 0xA1, bRequest 0x01, report type 3 in
 * wValue's high byte); -1 for anything else. */
static inline int ctrl_feature_rid(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue)
{
    if (bmRequestType != 0xA1 || bRequest != 0x01 || (wValue >> 8) != 3) return -1;
    return wValue & 0xFF;
}

/* Per report ID, for one attachment: cancelled on its last attempt, and
 * dropped. Zeroed at every enumeration. */
typedef struct {
    uint32_t cancelled[8];
    uint32_t dropped[8];
} report_drop_t;

static inline bool report_is_dropped(const report_drop_t *d, uint8_t rid)
{
    return (d->dropped[rid >> 5] >> (rid & 31)) & 1u;
}

/* Report `rid` answered: its run of cancels is over. */
static inline void report_drop_note_answer(report_drop_t *d, uint8_t rid)
{
    d->cancelled[rid >> 5] &= ~(1u << (rid & 31));
}

/* A request for report `rid` was cancelled. True if that drops it now. */
static inline bool report_drop_note_cancel(report_drop_t *d, uint8_t rid)
{
    uint32_t bit = 1u << (rid & 31);
    if (d->dropped[rid >> 5] & bit) return false;
    if (CTRL_CANCELS_BEFORE_DROP <= 1 || (d->cancelled[rid >> 5] & bit)) {
        d->dropped[rid >> 5] |= bit;
        return true;
    }
    d->cancelled[rid >> 5] |= bit;
    return false;
}

#endif /* UPSA_LINK_RECOVERY_H */
