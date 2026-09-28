/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-side test for hid_parser.c against a real captured report descriptor.
 *
 * Runs the firmware's own parser on captures/<dir>/report-descriptor.bin and
 * prints the field map plus the NUT variables it would publish. Compare the
 * output against tools/decode-hid.py, which is the independent reference
 * implementation: the two disagreeing means one of them is wrong.
 *
 * Needs no ESP-IDF and no hardware.
 */
#include "hid_parser.h"
#include "ups_varmap.h"
#include "ups_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *type_name(hid_item_type_t t)
{
    switch (t) {
    case HID_ITEM_INPUT:   return "Input";
    case HID_ITEM_OUTPUT:  return "Output";
    case HID_ITEM_FEATURE: return "Feature";
    default:               return "?";
    }
}

static const char *coll_name(uint16_t c)
{
    switch (c) {
    case HID_COLL_BATTERY_SYSTEM:  return "BatterySystem";
    case HID_COLL_BATTERY:         return "Battery";
    case HID_COLL_CHARGER:         return "Charger";
    case HID_COLL_POWER_CONVERTER: return "PowerConverter";
    case HID_COLL_OUTLET_SYSTEM:   return "OutletSystem";
    case HID_COLL_INPUT:           return "Input";
    case HID_COLL_OUTPUT:          return "Output";
    case HID_COLL_FLOW:            return "Flow";
    case HID_COLL_OUTLET:          return "Outlet";
    case HID_COLL_POWER_SUMMARY:   return "PowerSummary";
    default:                       return "-";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <report-descriptor.bin> [vid] [pid]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t buf[8192];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    uint16_t vid = (argc > 2) ? (uint16_t)strtol(argv[2], NULL, 16) : 0;
    uint16_t pid = (argc > 3) ? (uint16_t)strtol(argv[3], NULL, 16) : 0;

    static hid_report_map_t map;
    size_t n = hid_parse_report_descriptor(buf, len, &map);
    if (vid) hid_apply_quirks(&map, vid, pid);

    printf("\n%zu descriptor bytes -> %zu fields\n\n", len, n);
    printf("%4s %-8s %5s %4s %-9s %-15s %6s %7s  %s\n",
           "RID", "Type", "Off", "Size", "Page", "Collection", "Exp", "Shift", "NUT name");
    printf("--------------------------------------------------------------------------------------------\n");

    size_t named = 0, feature = 0, input = 0;
    for (size_t i = 0; i < map.count; i++) {
        const hid_field_t *fl = &map.fields[i];
        const ups_varmap_t *v = ups_varmap_lookup(fl);
        if (v) named++;
        if (fl->item_type == HID_ITEM_FEATURE) feature++;
        if (fl->item_type == HID_ITEM_INPUT) input++;
        printf("%4u %-8s %5u %4u %-9s %-15s %6d %7d  %s\n",
               fl->report_id, type_name(fl->item_type), fl->bit_offset, fl->bit_size,
               fl->usage_page == HID_PAGE_POWER_DEVICE ? "Power" : "Battery",
               coll_name(fl->collection), fl->unit_exponent, fl->decimal_shift,
               v ? v->nut_name : "");
    }

    printf("\nFeature %zu, Input %zu, mapped to NUT names %zu\n", feature, input, named);

    /* Distinct NUT variables, which is what a client would actually see. */
    printf("\nNUT variables this device would publish:\n");
    size_t vc = 0;
    const ups_varmap_t *tbl = ups_varmap_table(&vc);
    size_t published = 0;
    for (size_t t = 0; t < vc; t++) {
        for (size_t i = 0; i < map.count; i++) {
            if (ups_varmap_lookup(&map.fields[i]) == &tbl[t]) {
                printf("  %-26s report %3u  %s\n",
                       tbl[t].nut_name, map.fields[i].report_id,
                       coll_name(map.fields[i].collection));
                published++;
                break;
            }
        }
    }
    printf("\n%zu distinct NUT variables\n", published);
    return 0;
}
