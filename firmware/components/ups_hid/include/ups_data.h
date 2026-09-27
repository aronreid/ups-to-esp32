/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one struct every output path reads from. ups_hid writes it, nut_server
 * and webui read it. Nothing else should hold UPS state.
 *
 * Fields are deliberately NUT-shaped (see docs/firmware.md for the mapping)
 * but carry no NUT formatting -- that belongs in nut_server.
 */
#ifndef UPSA_UPS_DATA_H
#define UPSA_UPS_DATA_H

#include <stdbool.h>
#include <stdint.h>

/* NAN-equivalent for "the UPS did not report this". Every consumer must check
 * ups_valid() before publishing a field: a UPS that omits a usage is normal,
 * and reporting 0.0 for it would be a lie that reaches Home Assistant. */
#define UPS_VALUE_ABSENT (-1.0f)

typedef enum {
    UPS_STATUS_UNKNOWN   = 0,
    UPS_STATUS_ONLINE    = 1 << 0,  /* OL  */
    UPS_STATUS_ONBATT    = 1 << 1,  /* OB  */
    UPS_STATUS_LOWBATT   = 1 << 2,  /* LB  */
    UPS_STATUS_CHARGING  = 1 << 3,  /* CHRG */
    UPS_STATUS_DISCHARGE = 1 << 4,  /* DISCHRG */
    UPS_STATUS_REPLACEBATT = 1 << 5, /* RB */
    UPS_STATUS_OVERLOAD  = 1 << 6,  /* OVER */
} ups_status_bits_t;

/* Controls the attached UPS actually implements. */
typedef enum {
    UPS_CAP_TEST        = 1 << 0,  /* Power 0x58 Test: start/abort a self-test */
    UPS_CAP_BEEPER      = 1 << 1,  /* Power 0x5A AudibleAlarmControl           */
    UPS_CAP_LOWBATT     = 1 << 2,  /* Battery 0x29 RemainingCapacityLimit      */
    UPS_CAP_SHUTDOWN    = 1 << 3,  /* Power 0x57 DelayBeforeShutdown           */
    /* Battery 0x85 ManufacturerDate. The descriptor says whether the usage
     * EXISTS, not whether a write to it sticks -- the parser does not record
     * the Constant flag, and plenty of UPSes accept the transfer and ignore
     * it. The only test is to write and read back. */
    UPS_CAP_BATTDATE    = 1 << 4,
} ups_cap_bits_t;

/* The Test usage is written with one table and read with a DIFFERENT one.
 *
 * Both come from NUT's usbhid-ups, which is the interoperable definition --
 * verified against the shipped driver binary rather than inferred from the HID
 * spec, because inferring it got this wrong. Read value 6 is "no test
 * initiated"; under the spec-shaped guess it looked like "error", and the
 * bench APC returns exactly 6 at rest. A status page that calls a healthy
 * battery FAILED is worse than one that says nothing. */
typedef enum {                  /* written, to start or stop a test */
    UPS_TEST_CMD_NONE   = 0,
    UPS_TEST_CMD_QUICK  = 1,
    UPS_TEST_CMD_DEEP   = 2,
    UPS_TEST_CMD_ABORT  = 3,
} ups_test_cmd_t;

typedef enum {                  /* read back, as the last known result */
    UPS_TEST_NO_TEST    = 0,
    UPS_TEST_PASSED     = 1,
    UPS_TEST_WARNING    = 2,
    UPS_TEST_ERROR      = 3,
    UPS_TEST_ABORTED    = 4,
    UPS_TEST_IN_PROGRESS= 5,
    UPS_TEST_NOT_RUN    = 6,
} ups_test_result_t;

/* Beeper states, from AudibleAlarmControl. */
#define UPS_BEEPER_DISABLED 1
#define UPS_BEEPER_ENABLED  2
#define UPS_BEEPER_MUTED    3

typedef struct {
    /* Identity, from string descriptors. */
    char     mfr[32];
    char     model[32];
    char     serial[32];
    uint16_t vid;
    uint16_t pid;

    /* Live readings. UPS_VALUE_ABSENT where unreported. */
    float battery_charge;     /* %      */
    float battery_runtime;    /* s      */
    float battery_voltage;    /* V      */
    float input_voltage;      /* V      */
    float input_frequency;    /* Hz     */
    float output_voltage;     /* V      */
    float ups_load;           /* %      */
    float ups_realpower;      /* W, only if the UPS actually meters it */
    float ups_realpower_nom;  /* W, nameplate rating */
    float ups_apparentpower;  /* VA, rarely reported */

    uint32_t status;          /* bitmask of ups_status_bits_t */

    /* What this particular UPS will let us DO, discovered from its report
     * descriptor at enumeration -- never from the model name. Most units
     * implement a subset, and the UI shows only what is here. */
    uint32_t caps;            /* bitmask of ups_cap_bits_t */

    /* Control values read back, so the page reflects the UPS rather than what
     * we last asked for. UPS_VALUE_ABSENT / 0 when the cap is missing. */
    float    lowbatt_limit;   /* %, RemainingCapacityLimit */
    uint8_t  beeper;          /* 1 disabled, 2 enabled, 3 muted */
    uint8_t  test_result;     /* ups_test_result_t */

    /* Battery manufacture date as "YYYY-MM-DD", or "" if not reported.
     *
     * This is ManufacturerDate, which NUT publishes as battery.mfr.date. It
     * tells you when the battery was last replaced ONLY if whoever replaced it
     * updated the record; on an original pack it is effectively the UPS's own
     * build date, which is still the number you want when deciding whether a
     * battery is old. */
    char     battery_mfr_date[11];

    /* Link health. */
    bool     attached;        /* device enumerated and responding */
    int64_t  last_update_us;  /* esp_timer_get_time() of last good poll */
    uint32_t poll_failures;   /* consecutive; drives VBUS recovery */
} ups_data_t;

static inline bool ups_valid(float v) { return v != UPS_VALUE_ABSENT; }

/* Best available power figure, in watts, and whether it was measured.
 *
 * Most consumer UPSes do not meter power. A captured APC Back-UPS RS 1000G
 * reports PercentLoad and ConfigActivePower but neither ActivePower nor
 * ApparentPower, so watts can only be derived as load% x nameplate. That
 * derivation inherits PercentLoad's 1% granularity: about 6W steps on a 600W
 * unit, and a load under ~30W reads as 0-5%. Useful for trend and headroom,
 * not for energy accounting.
 *
 * Sets *estimated to true when the value was derived rather than measured, so
 * callers can label it honestly. Returns UPS_VALUE_ABSENT when neither route is
 * available -- never a fabricated zero. */
static inline float ups_power_watts(const ups_data_t *d, bool *estimated)
{
    if (ups_valid(d->ups_realpower)) {
        if (estimated) *estimated = false;
        return d->ups_realpower;
    }
    if (ups_valid(d->ups_load) && ups_valid(d->ups_realpower_nom)) {
        if (estimated) *estimated = true;
        return d->ups_realpower_nom * (d->ups_load / 100.0f);
    }
    if (estimated) *estimated = false;
    return UPS_VALUE_ABSENT;
}

#endif /* UPSA_UPS_DATA_H */
