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
 * The state lives in RTC memory, which survives a panic restart and not a
 * power cycle. Kept free of ESP-IDF so tools/test/test_usb_guard.py can run
 * the decision on the host.
 */
#ifndef UPSA_USB_GUARD_H
#define UPSA_USB_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#define USB_GUARD_MAGIC      0x55534247u     /* "USBG" */
#define USB_GUARD_NO_POLL_AT 3
#define USB_GUARD_OFF_AT     5
#define USB_GUARD_LIVE_US    (120LL * 1000000LL)

typedef enum {
    USB_GUARD_NORMAL  = 0,
    USB_GUARD_NO_POLL = 1,      /* enumerate, never poll */
    USB_GUARD_USB_OFF = 2,      /* no USB host, VBUS off */
} usb_guard_level_t;

typedef struct {
    uint32_t magic;
    uint8_t  crashes;   /* consecutive crashes with a UPS freshly attached */
    uint8_t  live;      /* a UPS attached less than USB_GUARD_LIVE_US ago */
} usb_guard_state_t;

/* Once per boot, before USB starts. `crashed`: the last reset was a panic or a
 * watchdog. Returns what this boot may do with USB. */
static inline usb_guard_level_t usb_guard_boot(usb_guard_state_t *g, bool crashed)
{
    if (g->magic != USB_GUARD_MAGIC) {          /* power-on: RTC is garbage */
        g->magic = USB_GUARD_MAGIC;
        g->crashes = 0;
    } else if (crashed && g->live) {
        if (g->crashes < 255) g->crashes++;
    } else {
        g->crashes = 0;                         /* clean restart, or unrelated */
    }
    g->live = 0;
    return g->crashes >= USB_GUARD_OFF_AT     ? USB_GUARD_USB_OFF
         : g->crashes >= USB_GUARD_NO_POLL_AT ? USB_GUARD_NO_POLL
         : USB_GUARD_NORMAL;
}

#endif
