/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Per-model profiles.
 *
 * The parser is generic and must stay that way: a UPS with no profile is
 * expected to work, because the USB HID Power Device class is a real standard
 * and most units follow it. A profile exists only to patch where a specific
 * model *deviates* from the standard -- a wrong unit exponent, a usage in the
 * wrong page, a value that needs inverting.
 *
 * So the rule is: never add a profile to make a UPS work. Add one only when a
 * UPS is observed to report something incorrectly, and record what was observed
 * in the note field. An empty profile table should still yield a working NUT
 * server for a conformant device.
 *
 * "Observed" may be upstream: a correction Network UPS Tools carries for a
 * real unit is evidence too, and the Tripp Lite entries are ported that way.
 * The note must then say it came from NUT and has not been measured here,
 * until it has.
 */
#ifndef UPSA_UPS_PROFILE_H
#define UPSA_UPS_PROFILE_H

#include <stdint.h>
#include "hid_parser.h"

/* Matches any product ID from a vendor. Use when a quirk is known to affect a
 * vendor's range and the exact PID list is not worth tracking. */
#define UPS_PID_ANY 0xFFFF

/* Tagged, not anonymous: hid_parser.h forward-declares `struct ups_profile`
 * to return one without pulling this header in. */
typedef struct ups_profile {
    uint16_t    vid;
    uint16_t    pid;        /* or UPS_PID_ANY */
    const char *name;       /* shown in logs, /api/status and ups.model */
    const char *note;       /* what was actually observed, and where */
    void      (*fixup)(hid_report_map_t *map);
} ups_profile_t;

/* Most specific match wins: exact VID+PID, then VID + UPS_PID_ANY, then NULL
 * for the generic path. NULL is a normal, expected outcome. */
const ups_profile_t *ups_profile_find(uint16_t vid, uint16_t pid);

/* Human-readable name of the matched profile, or "generic". */
const char *ups_profile_name(const ups_profile_t *p);

#endif /* UPSA_UPS_PROFILE_H */
