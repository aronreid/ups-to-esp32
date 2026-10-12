/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_vars.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int ups_vars_sub_for(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < ups_vartab_id_count; i++) {
        if (ups_vartab_ids[i].vid == vid && ups_vartab_ids[i].pid == pid) return ups_vartab_ids[i].sub;
    }
    for (size_t i = 0; i < ups_vartab_id_count; i++) {
        if (ups_vartab_ids[i].vid == vid) return ups_vartab_ids[i].sub;
    }
    return -1;
}

bool ups_vars_row_matches(const hid_report_map_t *map, const hid_field_t *f,
                          const ups_vartab_row_t *r)
{
    if ((((uint32_t)f->usage_page << 16) | f->usage) != r->node[r->depth - 1]) return false;
    const hid_path_t *p = &map->paths[f->path];
    if (p->depth != r->depth - 1) return false;
    return memcmp(p->node, r->node, p->depth * sizeof(uint32_t)) == 0;
}

bool ups_vars_st_matches(const hid_report_map_t *map, const hid_field_t *f,
                         const ups_st_row_t *r)
{
    if ((((uint32_t)f->usage_page << 16) | f->usage) != r->node[r->depth - 1]) return false;
    const hid_path_t *p = &map->paths[f->path];
    if (p->depth != r->depth - 1) return false;
    return memcmp(p->node, r->node, p->depth * sizeof(uint32_t)) == 0;
}

/* A number NUT would publish, unless it is one no working UPS reports. NUT's
 * own device dumps include an Eaton at 41216 V and a CyberPower at 120%
 * charge; a reading like that is dropped here rather than handed to upsmon. */
static bool fmt_num(const char *name, double v, int dec, char *out, size_t cap)
{
    if (isnan(v) || isinf(v)) return false;
    double lo = -1e9, hi = 1e9;
    if (strstr(name, ".voltage"))                  { lo = 0;   hi = 1000; }
    else if (strstr(name, ".frequency"))           { lo = 0;   hi = 100; }
    else if (strstr(name, ".current"))             { lo = 0;   hi = 1000; }
    else if (strstr(name, "temperature"))          { lo = -40; hi = 150; }
    else if (!strncmp(name, "battery.charge", 14)) { lo = 0;   hi = 100; }
    else if (!strcmp(name, "ups.load"))            { lo = 0;   hi = 300; }
    else if (strstr(name, "power"))                { lo = 0;   hi = 100000; }
    if (v < lo || v > hi) return false;
    snprintf(out, cap, "%.*f", dec, v);
    return true;
}

void ups_vars_state_init(ups_vars_state_t *st, uint16_t vid, uint16_t pid,
                         bool (*lookup)(void *, const char *, double *), void *ctx)
{
    memset(st, 0, sizeof(*st));
    /* cps-hid.c enables its scale corrections for exactly one ID, through
     * cps_battery_scale() in its device table. */
    st->cps_scale = (vid == 0x0764 && pid == 0x0501);
    st->batt_scale = 1.0;
    st->freq_scale[0] = st->freq_scale[1] = 1.0;
    st->lookup = lookup;
    st->lookup_ctx = ctx;
}

static double lookup(ups_vars_state_t *st, const char *name)
{
    double v = 0;
    if (st->lookup && st->lookup(st->lookup_ctx, name, &v)) return v;
    return 0;
}

/* cps-hid.c cps_adjust_frequency_scale(), line for line: decide once whether
 * this unit reports hertz or tenths of a hertz, from the nominal frequency
 * and range if it has them, else the 50 Hz and 60 Hz bands. */
static void cps_freq_decide(ups_vars_state_t *st, double freq, int out)
{
    if (st->freq_checked[out]) return;
    double nom  = lookup(st, out ? "output.frequency.nominal" : "input.frequency.nominal");
    double low  = lookup(st, out ? "output.frequency.low"     : "input.frequency.low");
    double high = lookup(st, out ? "output.frequency.high"    : "input.frequency.high");
    if (nom == 0) {
        if      (45 < low && low <= 50)          nom = 50;
        else if (50 <= high && high <= 55)       nom = 50;
        else if (45 < freq && freq <= 55)        nom = 50;
        else if (450 < freq && freq <= 550)      nom = 50;
        else if (55 < low && low <= 60)          nom = 60;
        else if (60 <= high && high <= 65)       nom = 60;
        else if (55 < freq && freq <= 65)        nom = 60;
        else if (550 < freq && freq <= 650)      nom = 60;
    }
    if (low == 0)  low  = (nom == 0) ? 45.0 : nom * 0.95;
    if (high == 0) high = (nom == 0) ? 65.0 : nom * 1.05;
    if (low <= freq && freq <= high) {
        st->freq_scale[out] = 1.0;  st->freq_checked[out] = true;
    } else if (low <= freq / 10.0 && freq / 10.0 <= high) {
        st->freq_scale[out] = 0.1;  st->freq_checked[out] = true;
    }                               /* else: undecided, try again next read */
}

static bool format_inner(ups_vars_state_t *st, const ups_vartab_row_t *r, const hid_field_t *f,
                         const uint8_t *pl, int len, char *out, size_t cap);

/* A battery voltage against its own nominal. Real packs sit near 0.85-1.2x;
 * outside 0.5-1.6x the reading is wrong, not the battery: NUT's own dumps
 * include a PR1500RT2U at 2.3 V on a 22 V pack, and a PowerWalker VI 750
 * (CyberPower 0601) reads 72 V on 12 V. Dropped rather than published. Both
 * numbers wrong by the same factor (an OL1000EXL: 402 on 360) cannot be told
 * apart from right here, and NUT publishes it the same. */
static bool battery_plausible(ups_vars_state_t *st, const char *out)
{
    double nom = lookup(st, "battery.voltage.nominal");
    if (nom <= 0) return true;
    double v = strtod(out, NULL);
    return v >= 0.5 * nom && v <= 1.6 * nom;
}

bool ups_vars_format(ups_vars_state_t *st, const ups_vartab_row_t *r, const hid_field_t *f,
                     const uint8_t *pl, int len, char *out, size_t cap)
{
    if (!format_inner(st, r, f, pl, len, out, cap)) return false;
    if (!strcmp(r->name, "battery.voltage") && !battery_plausible(st, out)) return false;
    return true;
}

static bool format_inner(ups_vars_state_t *st, const ups_vartab_row_t *r, const hid_field_t *f,
                         const uint8_t *pl, int len, char *out, size_t cap)
{
    double v = hid_extract(f, pl, (size_t)len);
    long iv = (long)v;
    switch (r->conv) {
    case VCONV_NONE:   return fmt_num(r->name, v, r->decimals, out, cap);
    case VCONV_DIV10:  return fmt_num(r->name, v * 0.1, 1, out, cap);
    case VCONV_DIV100: return fmt_num(r->name, v * 0.01, 1, out, cap);
    case VCONV_BEEPER: {
        static const char *w[] = { NULL, "disabled", "enabled", "muted" };
        if (iv < 1 || iv > 3) return false;
        snprintf(out, cap, "%s", w[iv]);
        return true;
    }
    case VCONV_TEST: {                      /* NUT's test_read_info, verbatim */
        static const char *w[] = { NULL, "Done and passed", "Done and warning",
            "Done and error", "Aborted", "In progress", "No test initiated", "Test scheduled" };
        if (iv < 1 || iv > 7) return false;
        snprintf(out, cap, "%s", w[iv]);
        return true;
    }
    case VCONV_YESNO:
        if (iv != 0 && iv != 1) return false;
        snprintf(out, cap, "%s", iv ? "yes" : "no");
        return true;
    case VCONV_ONOFF:
        if (iv != 0 && iv != 1) return false;
        snprintf(out, cap, "%s", iv ? "on" : "off");
        return true;
    case VCONV_DATE:                        /* NUT's date_conversion_fun */
        if (iv == 0) { snprintf(out, cap, "not set"); return true; }
        snprintf(out, cap, "%04ld/%02ld/%02ld", 1980 + (iv >> 9), (iv >> 5) & 0x0F, iv & 0x1F);
        return true;
    case VCONV_KELVIN:                      /* 273..373 is Kelvin, else Celsius */
        return fmt_num(r->name, (v >= 273 && v <= 373) ? v - 273.15 : v, 1, out, cap);
    case VCONV_HEX:
        snprintf(out, cap, "%08lx", (unsigned long)iv);
        return true;
    case VCONV_CPS_BATTCHARGE:             /* cps_battcharge_fun */
        return fmt_num(r->name, v < 100.0 ? v : 100.0, 0, out, cap);
    case VCONV_CPS_BATTVOLT:               /* cps_battvolt_fun */
        if (st->cps_scale && !st->batt_checked) {
            /* cps_adjust_battery_scale(). NUT publishes the unscaled value
             * until the nominal is known; a value that may be 1.5x wrong is
             * held back here instead. */
            double nom = lookup(st, "battery.voltage.nominal");
            if (nom == 0) return false;
            if (v / nom > 1.4) st->batt_scale = 2.0 / 3;
            st->batt_checked = true;
        }
        return fmt_num(r->name, st->batt_scale * v, 1, out, cap);
    case VCONV_CPS_INFREQ:
    case VCONV_CPS_OUTFREQ: {              /* cps_input_freq_fun, cps_output_freq_fun */
        int o = (r->conv == VCONV_CPS_OUTFREQ);
        if (st->cps_scale) cps_freq_decide(st, v, o);
        return fmt_num(r->name, st->freq_scale[o] * v, 1, out, cap);
    }
    case VCONV_STRINGID:
    default:
        return false;
    }
}
