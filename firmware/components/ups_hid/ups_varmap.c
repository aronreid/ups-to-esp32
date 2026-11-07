/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_varmap.h"
#include <stddef.h>

#define PWR HID_PAGE_POWER_DEVICE
#define BAT HID_PAGE_BATTERY_SYSTEM
#define ANY HID_COLL_NONE

/* Ordered so that collection-qualified entries are matched before wildcards.
 * Names follow the NUT variable namespace; see docs/firmware.md. */
static const ups_varmap_t s_map[] = {
    /* ---- Input ------------------------------------------------------- */
    { PWR, 0x30, HID_COLL_INPUT,           "input.voltage",            UPS_FMT_DEC1 },
    { PWR, 0x40, HID_COLL_INPUT,           "input.voltage.nominal",    UPS_FMT_INT  },
    { PWR, 0x31, HID_COLL_INPUT,           "input.current",            UPS_FMT_DEC2 },
    { PWR, 0x32, HID_COLL_INPUT,           "input.frequency",          UPS_FMT_DEC1 },
    { PWR, 0x42, HID_COLL_INPUT,           "input.frequency.nominal",  UPS_FMT_INT  },
    { PWR, 0x53, HID_COLL_INPUT,           "input.transfer.low",       UPS_FMT_INT  },
    { PWR, 0x54, HID_COLL_INPUT,           "input.transfer.high",      UPS_FMT_INT  },

    /* ---- Output ------------------------------------------------------ */
    { PWR, 0x30, HID_COLL_OUTPUT,          "output.voltage",           UPS_FMT_DEC1 },
    { PWR, 0x40, HID_COLL_OUTPUT,          "output.voltage.nominal",   UPS_FMT_INT  },
    { PWR, 0x31, HID_COLL_OUTPUT,          "output.current",           UPS_FMT_DEC2 },
    { PWR, 0x32, HID_COLL_OUTPUT,          "output.frequency",         UPS_FMT_DEC1 },

    /* ---- Battery ----------------------------------------------------- */
    { PWR, 0x30, HID_COLL_BATTERY,         "battery.voltage",          UPS_FMT_DEC2 },
    { PWR, 0x40, HID_COLL_BATTERY,         "battery.voltage.nominal",  UPS_FMT_INT  },
    { PWR, 0x31, HID_COLL_BATTERY,         "battery.current",          UPS_FMT_DEC2 },
    { PWR, 0x36, HID_COLL_BATTERY,         "battery.temperature",      UPS_FMT_DEC1 },
    { BAT, 0x66, ANY,                      "battery.charge",           UPS_FMT_INT  },
    { BAT, 0x29, ANY,                      "battery.charge.low",       UPS_FMT_INT  },
    { BAT, 0x68, ANY,                      "battery.runtime",          UPS_FMT_INT  },
    { BAT, 0x83, ANY,                      "battery.capacity",         UPS_FMT_INT  },
    { BAT, 0x67, ANY,                      "battery.capacity.full",    UPS_FMT_INT  },
    { BAT, 0x89, ANY,                      "battery.type",             UPS_FMT_STRING },
    { BAT, 0x28, ANY,                      "battery.mfr.date",         UPS_FMT_STRING },

    /* ---- UPS as a whole ---------------------------------------------- */
    { PWR, 0x35, ANY,                      "ups.load",                 UPS_FMT_INT  },
    { PWR, 0x34, ANY,                      "ups.realpower",            UPS_FMT_INT  },
    { PWR, 0x44, ANY,                      "ups.realpower.nominal",    UPS_FMT_INT  },
    { PWR, 0x33, ANY,                      "ups.power",                UPS_FMT_INT  },
    { PWR, 0x43, ANY,                      "ups.power.nominal",        UPS_FMT_INT  },
    { PWR, 0x36, ANY,                      "ups.temperature",          UPS_FMT_DEC1 },
    { PWR, 0x57, ANY,                      "ups.delay.shutdown",       UPS_FMT_INT  },
    { PWR, 0x56, ANY,                      "ups.delay.start",          UPS_FMT_INT  },
    { PWR, 0x55, ANY,                      "ups.delay.reboot",         UPS_FMT_INT  },
    { PWR, 0x58, ANY,                      "ups.test.result",          UPS_FMT_INT  },
    { PWR, 0x5A, ANY,                      "ups.beeper.status",        UPS_FMT_INT  },
    { PWR, 0xFD, ANY,                      "ups.mfr",                  UPS_FMT_STRING },
    { PWR, 0xFE, ANY,                      "ups.model",                UPS_FMT_STRING },
    { PWR, 0xFF, ANY,                      "ups.serial",               UPS_FMT_STRING },
};

#define MAP_COUNT (sizeof(s_map) / sizeof(s_map[0]))

const ups_varmap_t *ups_varmap_lookup(const hid_field_t *f)
{
    if (!f) return NULL;

    /* Exact collection match first, so input.voltage wins over any wildcard. */
    for (size_t i = 0; i < MAP_COUNT; i++) {
        if (s_map[i].usage_page == f->usage_page &&
            s_map[i].usage == f->usage &&
            s_map[i].collection == f->collection &&
            s_map[i].collection != ANY) {
            return &s_map[i];
        }
    }
    for (size_t i = 0; i < MAP_COUNT; i++) {
        if (s_map[i].usage_page == f->usage_page &&
            s_map[i].usage == f->usage &&
            s_map[i].collection == ANY) {
            return &s_map[i];
        }
    }
    return NULL;   /* no NUT equivalent: still published raw over HTTP/MQTT */
}

const ups_varmap_t *ups_varmap_table(size_t *count)
{
    if (count) *count = MAP_COUNT;
    return s_map;
}
