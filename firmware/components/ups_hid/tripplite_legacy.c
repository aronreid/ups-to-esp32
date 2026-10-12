/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tripp Lite legacy protocol decoding. See tripplite_legacy.h for what this
 * is, and for the NUT authors whose drivers/tripplite_usb.c it is ported from
 * (GPL-2.0-or-later). Plain C with no ESP-IDF, so the host tests run it.
 */
#include "tripplite_legacy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* NUT's battery voltage interval for the charge estimate, 12 V basis. */
#define TL_MIN_VOLT 11.0
#define TL_MAX_VOLT 13.4

tl_model_t tl_decode_protocol(uint16_t proto)
{
    switch (proto) {
    case 0x0004: return TL_SMART_0004;
    case 0x1001: return TL_OMNIVS;
    case 0x2001: return TL_OMNIVS_2001;
    case 0x3003: return TL_SMARTPRO;
    case 0x3005: return TL_SMART_3005;
    case 0x3017: return TL_SMART_3017;
    default:     return TL_UNKNOWN;
    }
}

const char *tl_model_name(tl_model_t m)
{
    switch (m) {
    case TL_OMNIVS:      return "OMNIVS (1001)";
    case TL_OMNIVS_2001: return "OMNIVS (2001)";
    case TL_SMARTPRO:    return "SMARTPRO (3003)";
    case TL_SMART_0004:  return "SMART (0004)";
    case TL_SMART_3005:  return "SMART binary (3005)";
    case TL_SMART_3017:  return "SMART (3017)";
    default:             return "unknown";
    }
}

bool tl_is_smart(tl_model_t m)
{
    return m == TL_SMARTPRO || m == TL_SMART_0004 || m == TL_SMART_3005 || m == TL_SMART_3017;
}

bool tl_is_binary(tl_model_t m)
{
    return m == TL_SMART_3005;
}

void tl_build_cmd(const uint8_t *msg, size_t len, uint8_t out[TL_PKT])
{
    uint8_t csum = 0;
    size_t i;
    if (len < 1) len = 1;
    if (len > 5) len = 5;
    memset(out, 0, TL_PKT);
    out[0] = ':';
    for (i = 0; i < len; i++) {
        out[i + 1] = msg[i];
        csum = (uint8_t)(csum + msg[i]);
    }
    /* NUT writes the checksum at index len, over the last message byte -- the
     * string literal's NUL -- and the carriage return after it. */
    out[len] = (uint8_t)(255 - csum);
    out[len + 1] = 13;
}

bool tl_reply_is_for(const uint8_t cmd_pkt[TL_PKT], const uint8_t reply[TL_PKT])
{
    return reply[0] == cmd_pkt[1];
}

uint16_t tl_protocol_from_reply(const uint8_t reply[TL_PKT])
{
    return (uint16_t)((reply[1] << 8) | reply[2]);
}

/* NUT's hex2d: up to len characters through strtol base 16, which stops at
 * the first non-hex character -- replies pad with 'X', '_' and NULs. */
static long hex2d(const uint8_t *start, unsigned len)
{
    char buf[32];
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return strtol(buf, NULL, 16);
}

static long bin2d(const uint8_t *start, unsigned len)
{
    long v = 0;
    for (unsigned i = 0; i < len; i++) v = (v << 8) | start[i];
    return v;
}

static long hex_or_bin2d(tl_model_t m, const uint8_t *start, unsigned len)
{
    return tl_is_binary(m) ? bin2d(start, len) : hex2d(start, len);
}

void tl_config_init(tl_config_t *c, tl_model_t model)
{
    c->model = model;
    c->battery_nominal = 12;
    c->input_nominal = 120;
    c->input_scaled = 120;
    c->load_banks = -1;
}

static void set_input(tl_config_t *c, int v)
{
    c->input_nominal = v;
    c->input_scaled = v;
}

void tl_decode_v(tl_config_t *c, const uint8_t v[TL_PKT])
{
    if (tl_is_binary(c->model)) {
        c->battery_nominal = (v[2] << 8) | v[3];        /* 0x00 0x0c -> 12 V */
    } else {
        c->battery_nominal = (int)hex2d(v + 2, 2) * 6;
    }

    uint8_t ivn = v[1];
    if (tl_is_smart(c->model) && c->model != TL_SMART_3017) {
        switch (ivn) {
        case 0: case '0': set_input(c, 100); break;
        case 1: case '1': set_input(c, 110); break;
        case 2: case '2': set_input(c, 120); break;
        case 3: case '3': set_input(c, 127); break;
        case 4: case '4': set_input(c, 208); break;
        case 5: case '5': set_input(c, 220); break;
        case 6: case '6': set_input(c, 230); break;
        case 7: case '7': set_input(c, 240); break;
        default: break;                                  /* NUT: warn, keep */
        }
    } else {
        /* NUT: "lots of odd cases here" -- regard with scepticism. */
        switch (ivn) {
        case '0': set_input(c, 100); break;
        case '1': set_input(c, 120); break;
        case '2': set_input(c, 230); break;              /* UK SMX1200XLHG, 3017 */
        case '3': c->input_nominal = 208; c->input_scaled = 230; break;
        case 6:   set_input(c, 230); break;
        default: break;
        }
    }

    uint8_t lb = v[4];
    if (lb >= '0' && lb <= '9')       c->load_banks = lb - '0';
    else if (tl_is_binary(c->model))  c->load_banks = lb;
}

void tl_reading_init(tl_reading_t *r)
{
    r->status = 0;
    r->stale = false;
    r->battery_v12 = TL_ABSENT;
    r->battery_voltage = TL_ABSENT;
    r->battery_charge = TL_ABSENT;
    r->input_voltage = TL_ABSENT;
    r->input_frequency = TL_ABSENT;
    r->output_voltage = TL_ABSENT;
    r->load = TL_ABSENT;
    r->temperature = TL_ABSENT;
}

void tl_decode_s(const tl_config_t *c, const uint8_t s[TL_PKT], tl_reading_t *r)
{
    tl_model_t m = c->model;

    if (m == TL_OMNIVS) {
        switch (s[2]) {
        case '0': r->status |= TL_ST_OL; break;
        case '1': r->status |= TL_ST_OB; break;
        case '2': r->status |= TL_ST_BYPASS; break;     /* charge-only mode */
        case '3': break;                                /* NUT: seen once, warns */
        default:  r->stale = true; break;
        }
    }

    if (tl_is_smart(m) || m == TL_OMNIVS_2001) {
        unsigned s2 = s[2];
        if (tl_is_binary(m)) s2 += '0';
        switch (s2) {
        case '0': break;                                /* battery OK */
        case '1': break;                                /* "battery bad - replace":
                                                         * NUT sets only a test status */
        case '2': r->status |= TL_ST_CAL; break;
        case '3': r->status |= TL_ST_OVER; break;
        case '4': break;
        case '5': r->status |= TL_ST_OVER; break;
        default:  r->stale = true; break;
        }
        if (s[4] & 4)      r->status |= TL_ST_OFF;
        else if (s[4] & 1) r->status |= TL_ST_OB;
        else               r->status |= TL_ST_OL;
    }

    unsigned s1 = s[1];
    if (tl_is_binary(m)) s1 += '0';
    switch (s1) {
    case '0': r->status |= TL_ST_LB; break;
    case '1': break;                                    /* depends on s[2] */
    case '2':
        if (m == TL_SMARTPRO) { r->status |= TL_ST_RB; break; }
        r->stale = true;
        break;
    default:  r->stale = true; break;
    }
}

void tl_decode_b(const tl_config_t *c, const uint8_t b[TL_PKT], tl_reading_t *r)
{
    r->input_voltage = (float)(hex2d(b + 1, 4) / 3600.0 * c->input_scaled);
    r->battery_v12 = (float)(hex2d(b + 5, 2) / 16.0);
    r->battery_voltage = r->battery_v12;                /* NUT: assumes 12 V */
}

void tl_decode_d(const tl_config_t *c, const uint8_t d[TL_PKT], tl_reading_t *r)
{
    /* Integer arithmetic, as NUT's %ld does. */
    r->input_voltage = (float)(hex_or_bin2d(c->model, d + 1, 2) * c->input_scaled / 120);
    r->battery_v12 = (float)(hex_or_bin2d(c->model, d + 3, 2) / 10.0);
    r->battery_voltage = (float)(r->battery_v12 * c->battery_nominal / 12.0);
}

void tl_decode_t(const tl_config_t *c, const uint8_t t[TL_PKT], tl_reading_t *r)
{
    if (c->model == TL_SMARTPRO || c->model == TL_SMART_3017) {
        r->input_frequency = (float)(hex2d(t + 3, 3) / 10.0);
    }
    if (c->model == TL_SMART_0004) {
        r->input_frequency = (float)(hex2d(t + 3, 4) / 10.0);
    }
    if (c->model == TL_SMART_3005) {
        r->temperature = (float)hex2d(t + 1, 1);
    } else {
        /* NUT: "I'm guessing this is a calibration constant of some sort." */
        r->temperature = (float)((unsigned)hex2d(t + 1, 2) * 0.3636 - 21);
    }
}

void tl_decode_l(const tl_config_t *c, const uint8_t l[TL_PKT], tl_reading_t *r)
{
    switch (c->model) {
    case TL_OMNIVS:
    case TL_OMNIVS_2001:
        r->output_voltage = (float)(hex2d(l + 1, 4) / 240.0 * c->input_scaled);
        break;
    case TL_SMARTPRO:
    case TL_SMART_3017:
    case TL_SMART_0004:
        r->load = (float)hex2d(l + 1, 2);
        break;
    case TL_SMART_3005:
        r->load = (float)hex_or_bin2d(c->model, l + 1, 1);
        break;
    default:
        break;
    }
}

void tl_estimate_charge(tl_reading_t *r)
{
    if (r->battery_v12 == TL_ABSENT) return;
    double bv = r->battery_v12;
    if (bv >= TL_MAX_VOLT)      r->battery_charge = 100;
    else if (bv <= TL_MIN_VOLT) r->battery_charge = 10;
    else r->battery_charge = (float)(int)(100 * sqrt((bv - TL_MIN_VOLT) / (TL_MAX_VOLT - TL_MIN_VOLT)));
}

size_t tl_poll_commands(tl_model_t m, char out[4])
{
    size_t n = 0;
    out[n++] = 'S';
    if (m == TL_OMNIVS || m == TL_OMNIVS_2001) out[n++] = 'B';
    if (tl_is_smart(m)) { out[n++] = 'D'; out[n++] = 'T'; }
    out[n++] = 'L';
    return n;
}
