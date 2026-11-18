/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * When this UPS's battery was fitted and when it is due for replacement
 * (GitHub #7).
 *
 * The fitted date comes from the UPS where it stores one (HID ManufacturerDate
 * on the battery, set from the page with UPS_CMD_BATTERY_DATE: most APC units).
 * Where it does not, the board keeps it, in NVS, against this UPS's USB IDs and
 * serial. The replacement is due a number of years after the fitted date
 * (default 3), or on a fixed date when one is set. Once that date has passed
 * the board says so: NUT ups.alarm, the page and /metrics. It never sets the
 * UPS's own RB status, which means the UPS itself measured a bad battery.
 *
 * Published as NUT battery.date (fitted) and battery.date.maintenance (due),
 * NUT's own names for these. Without a set clock nothing is due.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ups_data.h"

typedef struct {
    char    fitted[11];    /* "YYYY-MM-DD", "" if unknown */
    bool    fitted_board;  /* the board keeps it: the UPS has no date of its own */
    bool    ups_has_date;  /* the UPS stores a fitted date (set it there, not here) */
    uint8_t years;         /* replace every this many years */
    char    due[11];       /* "YYYY-MM-DD", "" if no fitted date and no fixed date */
    bool    due_fixed;     /* due is a date the owner set, not fitted + years */
    bool    clock;         /* the board knows today's date */
    bool    overdue;       /* clock && due && today >= due */
    int32_t days_left;     /* due - today; meaningful only when clock && due[0] */
} battdate_t;

/* Everything above for the UPS described by d. Cheap: served from a cache. */
void battdate_get(const ups_data_t *d, battdate_t *out);

/* Change this UPS's record. NULL leaves a field as it is; "" clears a date;
 * years < 0 leaves the interval. A fitted date is refused for a UPS that
 * stores its own (the page writes that one to the UPS). */
esp_err_t battdate_set(const ups_data_t *d, const char *fitted, int years,
                       const char *due, char *why, size_t why_cap);

/* The ups.alarm text when overdue, else NULL. */
const char *battdate_alarm(const battdate_t *b);
