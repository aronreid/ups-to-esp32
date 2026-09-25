/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_profile.h"
/* No logging here yet: the only profile is a placeholder that does nothing.
 * Add esp_log.h back with the first real fixup. */

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
    /* Nothing for APC (0x051D). A Back-UPS RS 1000G was captured and parses
     * correctly on the generic path, which is the intended outcome. Same
     * expectation for Eaton/MGE (0x0463), Tripp Lite (0x09AE) and
     * PowerCOM (0x0D9F) until a capture proves otherwise. */
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
