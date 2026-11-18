/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keeps one bad UPS from taking the board off the network.
 *
 * A UPS that trips an assert in ESP-IDF's USB host driver -- an APC BE600M1
 * did, on v0.11, on its first feature poll -- restarts the board, which finds
 * the UPS again and restarts again, every couple of seconds, forever. A board
 * in that loop cannot be reached, cannot report what is wrong, and cannot
 * install the update that fixes it. This firmware cannot audit every UPS that
 * will ever be plugged in, so it counts instead.
 *
 * A crash counts only if a UPS attached less than USB_GUARD_LIVE_US before it.
 * After USB_GUARD_NO_POLL_AT of those in a row the board still enumerates the
 * UPS -- its name and report descriptor, so the page can say what it is and
 * the descriptor can be sent to the developer -- but never polls it. After
 * USB_GUARD_OFF_AT it does not start USB at all, and switches VBUS off. Any
 * clean restart (an update, the page's Try again, a reboot) or a power cycle
 * forgets all of it and tries the UPS again.
 *
 * Two more ways a UPS can keep a board from staying up, neither of them a
 * crash within USB_GUARD_LIVE_US of attaching:
 *
 *  - ups_task hangs inside the USB host driver and the supervisor restarts
 *    the board. That is a software reset, not a panic, so it was never
 *    counted: a UPS that hung the task soon after every boot restarted the
 *    board every minute for ever, and it could never finish an update. A
 *    hang within USB_GUARD_HUNG_SOON_US of boot now counts as a crash. One
 *    long after boot does not: the odd hang weeks apart must not add up to
 *    a board that stops reading its UPS.
 *
 *  - a crash while the board is recovering a UPS that went quiet, minutes
 *    after it attached (v0.20.1 did this in the port power cycle, on a
 *    customer's board, in a loop). After USB_GUARD_RECOVER_AT of those the
 *    port power cycle is skipped, back to re-enumerating in place, until a
 *    clean restart. The UPS is still read.
 *
 * The state lives in RTC memory, which survives a panic restart and not a
 * power cycle. Kept free of ESP-IDF so tools/test/test_usb_guard.py can run
 * the decision on the host.
 */
#ifndef UPSA_USB_GUARD_H
#define UPSA_USB_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#define USB_GUARD_MAGIC      0x55534248u     /* "USBH": the layout grew, older state is dropped */
#define USB_GUARD_NO_POLL_AT 3
#define USB_GUARD_OFF_AT     5
#define USB_GUARD_LIVE_US    (120LL * 1000000LL)
#define USB_GUARD_HUNG_SOON_US (300LL * 1000000LL)
#define USB_GUARD_RECOVER_AT 2

typedef enum {
    USB_GUARD_NORMAL  = 0,
    USB_GUARD_NO_POLL = 1,      /* enumerate, never poll */
    USB_GUARD_USB_OFF = 2,      /* no USB host, VBUS off */
} usb_guard_level_t;

typedef struct {
    uint32_t magic;
    uint8_t  crashes;   /* consecutive crashes with a UPS freshly attached */
    uint8_t  live;      /* a UPS attached less than USB_GUARD_LIVE_US ago */
    uint8_t  hung;      /* this restart is for a hung ups_task, soon after boot */
    uint8_t  recovering;        /* a link recovery is under way */
    uint8_t  recover_crashes;   /* crashes during one, since a clean restart */
} usb_guard_state_t;

/* Once per boot, before USB starts. `crashed`: the last reset was a panic or a
 * watchdog. Returns what this boot may do with USB. */
static inline usb_guard_level_t usb_guard_boot(usb_guard_state_t *g, bool crashed)
{
    if (g->magic != USB_GUARD_MAGIC) {          /* power-on: RTC is garbage */
        g->magic = USB_GUARD_MAGIC;
        g->crashes = 0;
        g->recover_crashes = 0;
    } else {
        if (crashed && g->recovering) {
            if (g->recover_crashes < 255) g->recover_crashes++;
        } else if (!crashed && !g->hung) {
            g->recover_crashes = 0;             /* a clean restart forgets it */
        }
        if ((crashed && g->live) || g->hung) {
            if (g->crashes < 255) g->crashes++;
        } else {
            g->crashes = 0;                     /* clean restart, or unrelated */
        }
    }
    g->live = g->hung = g->recovering = 0;
    return g->crashes >= USB_GUARD_OFF_AT     ? USB_GUARD_USB_OFF
         : g->crashes >= USB_GUARD_NO_POLL_AT ? USB_GUARD_NO_POLL
         : USB_GUARD_NORMAL;
}

/* Just before restarting the board for a hung ups_task. */
static inline void usb_guard_note_hung(usb_guard_state_t *g, int64_t uptime_us)
{
    if (g->magic == USB_GUARD_MAGIC) g->hung = uptime_us < USB_GUARD_HUNG_SOON_US;
}

/* Around a link recovery, so a crash inside it is known for what it was. */
static inline void usb_guard_recovering(usb_guard_state_t *g, bool on)
{
    if (g->magic == USB_GUARD_MAGIC) g->recovering = on;
}

/* May this boot power-cycle the USB port to recover a quiet UPS? */
static inline bool usb_guard_port_cycle_ok(const usb_guard_state_t *g)
{
    return g->recover_crashes < USB_GUARD_RECOVER_AT;
}

#endif
