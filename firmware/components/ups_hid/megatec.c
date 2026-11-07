/* SPDX-License-Identifier: GPL-3.0-or-later
 * Megatec Q1 decoding. See megatec.h. */
#include "megatec.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* One whitespace-separated field into *v: a number, or '@' (not reported). */
static bool field(const char **p, const char *end, float *v)
{
    while (*p < end && **p == ' ') (*p)++;
    if (*p >= end) return false;
    const char *start = *p;
    while (*p < end && **p != ' ' && **p != '\r' && **p != '\n') (*p)++;
    size_t len = (size_t)(*p - start);
    if (len == 0 || len > 12) return false;
    if (start[0] == '@') {
        for (size_t i = 0; i < len; i++) if (start[i] != '@' && start[i] != '.') return false;
        *v = MQ_ABSENT;
        return true;
    }
    char buf[16];
    memcpy(buf, start, len);
    buf[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)buf[i]) && buf[i] != '.' && buf[i] != '-') return false;
    }
    char *e = NULL;
    double d = strtod(buf, &e);
    if (e != buf + len || !isfinite(d)) return false;
    *v = (float)d;
    return true;
}

/* The reply up to its '\r' (or n). */
static const char *line_end(const char *s, size_t n)
{
    const char *cr = memchr(s, '\r', n);
    return cr ? cr : s + n;
}

bool mq_parse_q1(const char *s, size_t n, mq_q1_t *out)
{
    if (!s || n < 2 || s[0] != '(') return false;
    const char *end = line_end(s, n);
    const char *p = s + 1;
    float f[7];
    for (int i = 0; i < 7; i++) {
        if (!field(&p, end, &f[i])) return false;
    }
    while (p < end && *p == ' ') p++;
    if (end - p < 8) return false;
    uint8_t bits = 0;
    for (int i = 0; i < 8; i++) {
        if (p[i] != '0' && p[i] != '1') return false;
        if (p[i] == '1') bits |= (uint8_t)(1u << (7 - i));
    }
    p += 8;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\0')) p++;
    if (p != end) return false;                 /* trailing junk: not a Q1 line */

    /* On mains, a real UPS sees some input and puts out some output. All
     * zeros there is the wrong dialect answering (NUT #1539), not a reading. */
    bool on_batt = bits & MQ_UTILITY_FAIL;
    bool nothing = f[0] <= 0.0f && f[2] <= 0.0f && f[4] <= 0.0f;
    if (!on_batt && nothing) return false;

    out->input_voltage       = f[0];
    out->input_fault_voltage = f[1];
    out->output_voltage      = f[2];
    out->load                = f[3];
    out->input_frequency     = f[4];
    out->battery_voltage     = f[5];
    out->temperature         = f[6];
    out->bits                = bits;
    return true;
}

bool mq_parse_rating(const char *s, size_t n, mq_rating_t *out)
{
    if (!s || n < 2 || s[0] != '#') return false;
    const char *end = line_end(s, n);
    const char *p = s + 1;
    float f[4];
    for (int i = 0; i < 4; i++) {
        if (!field(&p, end, &f[i])) return false;
    }
    out->voltage = f[0];
    out->current = f[1];
    out->battery_voltage = f[2];
    out->frequency = f[3];
    return true;
}

static void trimmed(const char *src, size_t len, char *dst, size_t cap)
{
    while (len && src[0] == ' ') { src++; len--; }
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\0')) len--;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c < 0x7F) dst[o++] = (char)c;
    }
    if (cap) dst[o] = '\0';
}

bool mq_parse_info(const char *s, size_t n, char *mfr, size_t mfr_cap,
                   char *model, size_t model_cap)
{
    if (!s || n < 2 || s[0] != '#') return false;
    const char *end = line_end(s, n);
    size_t len = (size_t)(end - (s + 1));
    if (len < 15) return false;
    trimmed(s + 1, 15, mfr, mfr_cap);
    trimmed(s + 16, len - 15 < 10 ? len - 15 : 10, model, model_cap);
    return mfr[0] || model[0];
}

float mq_charge_estimate(float bv, float nominal)
{
    if (bv <= 0.0f) return MQ_ABSENT;
    if (nominal <= 0.0f) nominal = 12.0f;
    /* Per cell or per pack: the multiplier that lands within the nominal's
     * range, the same set NUT tries. */
    static const float packs[] = { 120, 100, 80, 60, 48, 36, 30, 24, 18, 12, 8, 6, 4, 3, 2, 1, 0.5f };
    float v = MQ_ABSENT;
    for (size_t i = 0; i < sizeof(packs) / sizeof(packs[0]); i++) {
        float t = bv * packs[i];
        if (t >= 0.8f * nominal && t <= 1.25f * nominal) { v = t; break; }
    }
    if (v == MQ_ABSENT) return MQ_ABSENT;
    float low = 104.0f * nominal / 120.0f, high = 130.0f * nominal / 120.0f;
    float c = 100.0f * (v - low) / (high - low);
    if (c < 0.0f) c = 0.0f;
    if (c > 100.0f) c = 100.0f;
    return roundf(c);
}

uint8_t mq_string_index(mq_dialect_t d, const char *cmd)
{
    if (!cmd) return 0;
    (void)d;    /* Q1, F and I share their indexes in every dialect here */
    if (strcmp(cmd, "Q1") == 0) return 0x03;
    if (strcmp(cmd, "F") == 0)  return 0x0d;
    if (strcmp(cmd, "I") == 0)  return 0x0c;
    return 0;
}

mq_xport_t mq_transport_for(uint16_t vid, uint16_t pid)
{
    static const struct { uint16_t vid, pid; mq_xport_t x; } t[] = {
        { 0x0001, 0x0000, MQ_XPORT_STRING  },   /* MEC0003 family (Fry's, PowerCool, Hunnox) */
        { 0x1cb0, 0x0035, MQ_XPORT_STRING  },   /* Legrand Daker DK (krauler indexes) */
        { 0x0665, 0x5161, MQ_XPORT_CYPRESS },   /* Cypress "USB to Serial" */
        { 0x05b8, 0x0000, MQ_XPORT_CYPRESS },
        { 0x06da, 0x0002, MQ_XPORT_CYPRESS },
        { 0x06da, 0x0004, MQ_XPORT_CYPRESS },
        { 0x06da, 0x0005, MQ_XPORT_CYPRESS },
        { 0x06da, 0x0201, MQ_XPORT_CYPRESS },
        { 0x0f03, 0x0001, MQ_XPORT_CYPRESS },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].vid == vid && t[i].pid == pid) return t[i].x;
    }
    return MQ_XPORT_NONE;
}

bool mq_hid_gives_status(const hid_report_map_t *map)
{
    /* Battery page: RemainingCapacity, RunTimeToEmpty, ACPresent,
     * BelowRemainingCapacityLimit, Charging, Discharging. */
    static const uint16_t bat[] = { 0x66, 0x68, 0xD0, 0x42, 0x44, 0x45 };
    for (size_t i = 0; i < sizeof(bat) / sizeof(bat[0]); i++) {
        if (hid_find(map, HID_PAGE_BATTERY_SYSTEM, bat[i])) return true;
    }
    return false;
}

bool mq_should_try(uint16_t vid, uint16_t pid, const hid_report_map_t *map)
{
    return mq_transport_for(vid, pid) != MQ_XPORT_NONE && !mq_hid_gives_status(map);
}
