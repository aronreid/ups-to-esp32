/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Battery dates, the pure part (GitHub #7): calendar arithmetic and the key a
 * UPS's record is stored under. No ESP-IDF, so tools/test/test_battdate.py
 * compiles it on the host and checks month ends, leap years and keying.
 *
 * Dates are whole days since 1970-01-01 (the proleptic Gregorian calendar, as
 * Howard Hinnant's days_from_civil), -1 for "none".
 */
#ifndef BATTDATE_CORE_H
#define BATTDATE_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BD_NONE        (-1)
#define BD_YEARS_MIN   1
#define BD_YEARS_MAX   10
#define BD_YEARS_DEF   3        /* lead-acid UPS batteries: 3 to 5 years */
#define BD_YEAR_MIN    1980     /* the HID packed date cannot go earlier */
#define BD_YEAR_MAX    2107     /* nor later */

static inline int32_t bd_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static inline void bd_civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

static inline bool bd_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static inline unsigned bd_month_len(int y, unsigned m)
{
    static const unsigned L[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && bd_leap(y)) ? 29 : L[m - 1];
}

/* "YYYY-MM-DD", a real date from 1980 to 2107, exactly ten characters. */
static inline bool bd_parse(const char *s, int32_t *days)
{
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    unsigned m = (unsigned)((s[5]-'0')*10 + (s[6]-'0'));
    unsigned d = (unsigned)((s[8]-'0')*10 + (s[9]-'0'));
    if (y < BD_YEAR_MIN || y > BD_YEAR_MAX || m < 1 || m > 12 || d < 1 || d > bd_month_len(y, m)) return false;
    *days = bd_days_from_civil(y, m, d);
    return true;
}

static inline void bd_format(int32_t days, char *out, size_t cap)
{
    if (days == BD_NONE) { if (cap) out[0] = '\0'; return; }
    int y; unsigned m, d;
    bd_civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%04d-%02u-%02u", y, m, d);
}

/* The same day `years` later; 29 February becomes 28 February in a common year. */
static inline int32_t bd_add_years(int32_t days, int years)
{
    if (days == BD_NONE) return BD_NONE;
    int y; unsigned m, d;
    bd_civil_from_days(days, &y, &m, &d);
    y += years;
    if (d > bd_month_len(y, m)) d = bd_month_len(y, m);
    return bd_days_from_civil(y, m, d);
}

/* Replace by: a fixed date if one is set, else fitted + years, else none. */
static inline int32_t bd_due(int32_t fitted, int years, int32_t fixed)
{
    if (fixed != BD_NONE) return fixed;
    if (fitted == BD_NONE) return BD_NONE;
    return bd_add_years(fitted, years);
}

/* Today from a Unix time, or BD_NONE when the clock has not been set (before
 * 2023: the board has no battery-backed clock and starts at 1970). */
static inline int32_t bd_today(int64_t unix_s)
{
    if (unix_s < 1700000000LL) return BD_NONE;
    return (int32_t)(unix_s / 86400);
}

/* The record belongs to one UPS: its USB IDs, its manufacturer and model
 * strings, and, where it reports one, its serial, so swapping the UPS does
 * not carry the date over. The model matters: CyberPowers share 0764:0501
 * across models and many report no serial (an EC850LCD and an ST Series on
 * one hub shared a record until the model was added). Two UPSes of the same
 * model with no serial cannot be told apart. FNV-1a, 32 bit; a 0 byte
 * separates the fields so "ab"+"c" and "a"+"bc" differ. */
static inline uint32_t bd_fnv(uint32_t h, const char *s)
{
    for (const char *p = s ? s : ""; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= 0; h *= 16777619u;
    return h;
}

static inline uint32_t bd_key_hash(uint16_t vid, uint16_t pid, const char *mfr,
                                   const char *model, const char *serial)
{
    char head[12];
    snprintf(head, sizeof(head), "%04x:%04x", vid, pid);
    uint32_t h = bd_fnv(2166136261u, head);
    h = bd_fnv(h, mfr);
    h = bd_fnv(h, model);
    return bd_fnv(h, serial);
}

#endif /* BATTDATE_CORE_H */
