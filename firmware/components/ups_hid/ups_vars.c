/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_vars.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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

bool ups_vars_format(const ups_vartab_row_t *r, const hid_field_t *f,
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
    case VCONV_STRINGID:
    default:
        return false;
    }
}
