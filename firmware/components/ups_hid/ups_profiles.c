/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_profile.h"
/* Profiles patch a parsed map; they log nothing. hid_apply_quirks() logs
 * which one matched. */

/* ---- CyberPower ------------------------------------------------------- */

static void cps_fixup(hid_report_map_t *map)
{
    /* Placeholder, deliberately doing nothing yet.
     *
     * NUT's cps-hid subdriver corrects output.voltage on several CPS models,
     * and this project originally carried that as a vendor quirk. Capturing a
     * real APC descriptor showed the usual cause of "1230 or 12.3 instead of
     * 123" is not vendor-specific at all: HID's SI Linear base units are
     * centimetres and grams, so a voltage exponent is relative to 10^7, and
     * hid_unit_decimal_offset() now handles that generically for every device.
     *
     * Whether CPS units additionally deviate is UNVERIFIED -- no CyberPower has
     * been captured yet. Capture an EC850LCD with tools/capture-ups.sh, compare
     * against a meter, and either delete this entry or implement a correction
     * with the measurement recorded in the note field. Do not guess. */
    (void)map;
}

/* ---- Tripp Lite ------------------------------------------------------- */
/*
 * Ported from Network UPS Tools' drivers/tripplite-hid.c (GPL-2.0-or-later),
 * which carries these per-product corrections from real units. NOT YET
 * MEASURED HERE: no Tripp Lite has been on this bench. The factors apply
 * unchanged because NUT and this parser scale raw values the same way -- by
 * 10^(exponent - 7) for volts, VA and watts, 10^exponent for amps and hertz
 * (libhid.c's HIDUnits table against hid_unit_decimal_offset()) -- so a NUT
 * factor of 0.1 is a decimal shift of -1 here, 100000 is +5, 0.01 is -2.
 *
 * Legacy 09ae:0001 units are not here: they speak a vendor protocol, not HID
 * Power Device, and never reach the parser. See tripplite_legacy.h.
 */

#define PWR HID_PAGE_POWER_DEVICE
#define BAT HID_PAGE_BATTERY_SYSTEM

static void shift_field(hid_report_map_t *map, uint16_t page, uint16_t usage,
                        uint16_t collection, int8_t delta)
{
    for (size_t i = 0; i < map->count; i++) {
        hid_field_t *f = &map->fields[i];
        if (f->usage_page == page && f->usage == usage && f->collection == collection) {
            f->decimal_shift = (int8_t)(f->decimal_shift + delta);
        }
    }
}

/* Some Tripp Lite units (OMNI1000LCD, per NUT) put four PresentStatus flags on
 * the Power Device page instead of Battery System: Charging 0x44, Discharging
 * 0x45, NeedReplacement 0x4B and ACPresent 0xD0. Read where the standard says,
 * a unit like that is never seen to lose mains -- and 0x84:0x44 is really
 * ConfigActivePower, so its 1-bit Charging flag would also be read as a
 * nameplate rating of 0 or 1 W. NUT recognises them only inside PresentStatus;
 * here that is a 1-bit field under PowerSummary, which the real Config*
 * values (multi-bit, in Flow) never are. Harmless on a unit that does it
 * right: such a field does not exist there. */
static void tl_status_page(hid_report_map_t *map)
{
    for (size_t i = 0; i < map->count; i++) {
        hid_field_t *f = &map->fields[i];
        if (f->usage_page == PWR && f->bit_size == 1 &&
            f->collection == HID_COLL_POWER_SUMMARY &&
            (f->usage == 0x44 || f->usage == 0x45 || f->usage == 0x4B || f->usage == 0xD0)) {
            f->usage_page = BAT;
        }
    }
}

/* battery_scale_0dot1: battery voltage reads ten times high. */
static void tl_battery_tenth(hid_report_map_t *map)
{
    tl_status_page(map);
    shift_field(map, PWR, 0x30, HID_COLL_BATTERY, -1);
}

/* NUT's workaround for a chemistry string the AVR550U (1003) and OMNI1000LCD
 * (2005) firmware gets wrong: report nothing rather than garbage. */
static void tl_battery_tenth_nochem(hid_report_map_t *map)
{
    tl_battery_tenth(map);
    for (size_t i = 0; i < map->count; i++) {
        if (map->fields[i].usage_page == BAT && map->fields[i].usage == 0x89) {
            map->fields[i].found = false;
        }
    }
}

/* smart1500lcdt_scale: battery and I/O voltage x100000, frequency and current
 * x0.01. */
static void tl_smart1500lcdt(hid_report_map_t *map)
{
    tl_status_page(map);
    shift_field(map, PWR, 0x30, HID_COLL_BATTERY, +5);
    shift_field(map, PWR, 0x30, HID_COLL_INPUT, +5);
    shift_field(map, PWR, 0x30, HID_COLL_OUTPUT, +5);
    shift_field(map, PWR, 0x32, HID_COLL_INPUT, -2);
    shift_field(map, PWR, 0x31, HID_COLL_OUTPUT, -2);
}

#define TL(pid_, fix_) { .vid = 0x09AE, .pid = (pid_), .name = "Tripp Lite", \
    .note = "NUT tripplite-hid.c correction, not yet measured on this bench", .fixup = (fix_) }

/* ---- Table ------------------------------------------------------------ */

/* Deliberately almost empty. The generic path is the product: descriptor
 * parsing plus unit-aware scaling covers conformant devices, which is most of
 * them. Add an entry only for a measured deviation. */
static const ups_profile_t s_profiles[] = {
    {
        .vid   = 0x0764,
        .pid   = UPS_PID_ANY,
        .name  = "CyberPower",
        .note  = "reserved pending a capture; the voltage-scaling problem this "
                 "was created for turned out to be generic HID unit handling, "
                 "so this entry may prove unnecessary",
        .fixup = cps_fixup,
    },
    /* Tripp Lite, per product. Protocols 1xxx-2xxx report battery voltage x10;
     * 3016/3024 need NUT's SMART1500LCDT set; every other Tripp Lite is
     * correct as reported and gets only the status-page fix, below. */
    TL(0x1003, tl_battery_tenth_nochem),
    TL(0x1007, tl_battery_tenth), TL(0x1008, tl_battery_tenth),
    TL(0x1009, tl_battery_tenth), TL(0x1010, tl_battery_tenth),
    TL(0x2005, tl_battery_tenth_nochem),
    TL(0x2007, tl_battery_tenth), TL(0x2008, tl_battery_tenth),
    TL(0x2009, tl_battery_tenth), TL(0x2010, tl_battery_tenth),
    TL(0x2011, tl_battery_tenth), TL(0x2012, tl_battery_tenth),
    TL(0x2013, tl_battery_tenth), TL(0x2014, tl_battery_tenth),
    TL(0x3016, tl_smart1500lcdt), TL(0x3024, tl_smart1500lcdt),
    {
        .vid   = 0x09AE,
        .pid   = UPS_PID_ANY,
        .name  = "Tripp Lite",
        .note  = "status flags moved to the Battery System page where a unit "
                 "misplaces them (NUT tripplite-hid.c); no scaling",
        .fixup = tl_status_page,
    },
    /* Nothing for APC (0x051D). A Back-UPS RS 1000G was captured and parses
     * correctly on the generic path, which is the intended outcome. Same
     * expectation for Eaton/MGE (0x0463) and PowerCOM (0x0D9F) until a
     * capture proves otherwise. */
};

#define PROFILE_COUNT (sizeof(s_profiles) / sizeof(s_profiles[0]))

const ups_profile_t *ups_profile_find(uint16_t vid, uint16_t pid)
{
    const ups_profile_t *wildcard = NULL;

    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        const ups_profile_t *p = &s_profiles[i];
        if (p->vid != vid) continue;
        if (p->pid == pid) return p;                  /* exact wins outright */
        if (p->pid == UPS_PID_ANY && !wildcard) wildcard = p;
    }
    return wildcard;   /* may be NULL: the generic path, which is fine */
}

const char *ups_profile_name(const ups_profile_t *p)
{
    return p ? p->name : "generic";
}
