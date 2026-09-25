/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal HID report descriptor parser, scoped to the Power Device page.
 *
 * This stays generic on purpose. The EC850LCD is the unit on the bench, but
 * hardcoding its report IDs would break every other UPS. We walk the
 * descriptor, find the usages we care about, and record where they live.
 */
#ifndef UPSA_HID_PARSER_H
#define UPSA_HID_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* USB HID usage pages relevant to a UPS. */
#define HID_PAGE_POWER_DEVICE   0x84
#define HID_PAGE_BATTERY_SYSTEM 0x85

/* Where a single logical value lives inside a report, and how to scale it. */
/* Which report a field lives in. A single report ID can carry both an Input and
 * a Feature report, and they have SEPARATE bit spaces -- observed on a real APC
 * Back-UPS, where report 12 has RemainingCapacity at bit 0 of both. Tracking one
 * cursor per report ID silently corrupts every offset after the first. */
typedef enum {
    HID_ITEM_INPUT = 0,
    HID_ITEM_OUTPUT,
    HID_ITEM_FEATURE,
    HID_ITEM_TYPE_COUNT,
} hid_item_type_t;

/* Significant Power Device collection usages. A value's meaning depends on
 * which collection encloses it: Voltage inside Input is input.voltage, inside
 * Battery it is battery.voltage. Confirmed on a real APC, where Voltage appears
 * three times under Input, Battery and PowerSummary. Without this the three are
 * indistinguishable. */
#define HID_COLL_BATTERY_SYSTEM  0x10
#define HID_COLL_BATTERY         0x12
#define HID_COLL_CHARGER         0x14
#define HID_COLL_POWER_CONVERTER 0x16
#define HID_COLL_OUTLET_SYSTEM   0x18
#define HID_COLL_INPUT           0x1A
#define HID_COLL_OUTPUT          0x1C
#define HID_COLL_FLOW            0x1E
#define HID_COLL_OUTLET          0x20
#define HID_COLL_POWER_SUMMARY   0x24
#define HID_COLL_NONE            0x00

typedef struct {
    uint16_t        usage_page;
    uint16_t        usage;
    uint16_t        collection;   /* nearest significant enclosing collection */
    uint8_t         report_id;
    hid_item_type_t item_type;
    uint16_t        bit_offset;   /* within its own report ID + type */
    uint16_t        bit_size;
    int8_t          unit_exponent;
    uint32_t        unit;         /* raw HID Unit field, needed for scaling */
    int8_t          decimal_shift;/* precomputed: value = raw * 10^shift */
    bool            is_signed;
    bool            found;
} hid_field_t;

/* A real APC Back-UPS RS 1000G yields 86 power/battery fields across 44 report
 * IDs, so 48 silently truncated. Sized with headroom for larger units. */
#define HID_MAX_FIELDS 160

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    size_t      count;
} hid_report_map_t;

/* Walk a raw report descriptor and record every Power Device / Battery System
 * field found. Returns the number of fields recorded. */
size_t hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                   hid_report_map_t *out);

/* Look up a parsed field by usage, preferring a Feature report because that is
 * where UPSes put authoritative values. NULL if the UPS does not report it. */
const hid_field_t *hid_find(const hid_report_map_t *map,
                            uint16_t usage_page, uint16_t usage);

/* As hid_find, but restricted to one report type. */
const hid_field_t *hid_find_typed(const hid_report_map_t *map,
                                  uint16_t usage_page, uint16_t usage,
                                  hid_item_type_t type);

/* Find a usage inside a specific collection. This is the lookup that
 * disambiguates Voltage between input, output and battery. */
const hid_field_t *hid_find_in(const hid_report_map_t *map,
                               uint16_t usage_page, uint16_t usage,
                               uint16_t collection);

/* Decimal offset between HID's base units and the conventional SI unit.
 *
 * HID SI Linear measures in centimetres and grams, not metres and kilograms, so
 * a quantity's reported exponent is relative to cm/g. offset = 2*len + 3*mass,
 * which gives 7 for volts and watts, 0 for amps, seconds, hertz and
 * dimensionless values. The true scale is then 10^(exponent - offset).
 *
 * This is the generic explanation for UPS voltages arriving as 1230 or 12.3
 * instead of 123. It is not vendor-specific and needs no per-model table. */
int hid_unit_decimal_offset(uint32_t unit);

/* Extract and scale one field from a received report payload. */
float hid_extract(const hid_field_t *f, const uint8_t *report, size_t len);

/* Look up this device's profile and apply any corrections it defines.
 *
 * Returns the matched profile, or NULL when the generic path applies -- the
 * expected outcome for a conformant UPS. Keyed on VID/PID, never on model
 * string. See ups_profile.h for how to add a model. */
struct ups_profile;
const struct ups_profile *hid_apply_quirks(hid_report_map_t *map,
                                           uint16_t vid, uint16_t pid);

#endif /* UPSA_HID_PARSER_H */
