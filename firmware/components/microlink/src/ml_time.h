/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The control plane's timestamps, parsed without trusting them. Header-only
 * so tools/test/test_ml_time.py can compile it on the host.
 *
 * The netmap is delivered by a server the board does not control, and Node
 * KeyExpiry arrives as text. The fields used to index a month table and bound
 * a year loop with no range check, so a month of 2147483647 read far past the
 * table and a year of 2147483647 looped for minutes. Anything out of range is
 * now "unparseable".
 */
#ifndef UPSA_ML_TIME_H
#define UPSA_ML_TIME_H

#include <stdint.h>
#include <stdio.h>

/* "YYYY-MM-DDTHH:MM:SS..." (UTC) -> seconds since 1970, or -1 if it is not a
 * plausible date: year 1970..2200, month 1..12, day 1..31, hour < 24,
 * minute < 60, second < 61. A fractional part or a zone after the seconds is
 * ignored; the control plane sends "Z". */
static inline int64_t iso8601_to_epoch(const char *s)
{
    int yr, mo, dy, hr, mn, sc;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) return -1;
    if (yr < 1970 || yr > 2200 || mo < 1 || mo > 12 || dy < 1 || dy > 31 ||
        hr < 0 || hr > 23 || mn < 0 || mn > 59 || sc < 0 || sc > 60) return -1;
    int64_t days = 0;
    for (int y = 1970; y < yr; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    static const int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < mo; m++) {
        days += mdays[m];
        if (m == 2 && (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0))) days++;
    }
    days += dy - 1;
    return days * 86400 + hr * 3600 + mn * 60 + sc;
}

#endif
