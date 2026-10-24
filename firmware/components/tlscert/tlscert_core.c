/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tlscert_core.h"
#include <stdio.h>
#include <string.h>

/* Days since 1970-01-01 of a civil date, and back (Howard Hinnant's). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

uint32_t tlsc_build_epoch(const char *date, const char *time)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "";
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (!date || !time) return 0;
    if (sscanf(date, "%3s %d %d", mon, &d, &y) != 3) return 0;
    if (sscanf(time, "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;
    const char *p = strstr(months, mon);
    if (!p || strlen(mon) != 3 || (p - months) % 3) return 0;
    unsigned m = (unsigned)((p - months) / 3 + 1);
    if (y < 2020 || y > 2105 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return 0;
    int64_t days = days_from_civil(y, m, (unsigned)d);
    return (uint32_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

void tlsc_x509_time(uint32_t epoch, char out[15])
{
    int y; unsigned m, d;
    civil_from_days(epoch / 86400, &y, &m, &d);
    uint32_t s = epoch % 86400;
    /* Through a roomier buffer: the compiler cannot see the fields' ranges. */
    char t[40];
    snprintf(t, sizeof(t), "%04d%02u%02u%02u%02u%02u", y, m, d,
             (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    memcpy(out, t, 14);
    out[14] = '\0';
}

uint32_t tlsc_ref_time(uint32_t clock_epoch, uint32_t build_epoch)
{
    uint32_t t = clock_epoch ? clock_epoch : build_epoch;
    return t > TLSC_EPOCH_FLOOR ? t : TLSC_EPOCH_FLOOR;
}

void tlsc_names(char *out, size_t cap, const char *host, const char *ip)
{
    snprintf(out, cap, "%s|%s", host ? host : "", ip ? ip : "");
}

bool tlsc_need_leaf(const char *names, uint32_t not_after,
                    const char *want_host, const char *want_ip, uint32_t now)
{
    if (!names) return true;
    const char *bar = strchr(names, '|');
    if (!bar) return true;
    size_t hl = (size_t)(bar - names);
    if (!want_host || strlen(want_host) != hl || strncmp(names, want_host, hl)) return true;
    bool ip_known = want_ip && want_ip[0] && strcmp(want_ip, "0.0.0.0");
    if (ip_known && strcmp(bar + 1, want_ip)) return true;
    if (now && (uint64_t)now + (uint64_t)TLSC_RENEW_DAYS * 86400 >= not_after) return true;
    return false;
}

void tlsc_hex_colons(const uint8_t sha[32], char out[96])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++) {
        out[i * 3]     = hex[sha[i] >> 4];
        out[i * 3 + 1] = hex[sha[i] & 15];
        out[i * 3 + 2] = i < 31 ? ':' : '\0';
    }
}
