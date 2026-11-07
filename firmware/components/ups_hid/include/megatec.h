/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Megatec "Q1": the text protocol of most low-cost UPSes that are not USB HID
 * Power Devices. Our own implementation of the published protocol
 * (https://networkupstools.org/protocols/megatec.html).
 *
 * This is the decoding half: replies to readings, nothing that touches USB, so
 * tools/test/test_megatec.py compiles and runs it on the host. The transports
 * (how the text travels over USB) are in ups_hid.c.
 *
 * Q1 asks for the readings:
 *     (MMM.M NNN.N PPP.P QQQ RR.R S.SS TT.T b7b6b5b4b3b2b1b0\r
 *     input V, input fault V, output V, load %, input Hz, battery V,
 *     temperature C, then eight status bits, b7 first.
 * F asks for the rating:  #MMM.M QQQ SS.SS RR.R\r
 *     nominal V, nominal current, nominal battery V, nominal Hz.
 * I asks for the identity: #<company 15><model 10><version 10>\r
 * '@' in a field means the UPS does not report it.
 */
#ifndef UPSA_MEGATEC_H
#define UPSA_MEGATEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hid_parser.h"

#define MQ_ABSENT (-1.0f)

/* Status bits, as numbered in the reply (b7 is the first character). */
#define MQ_UTILITY_FAIL  (1u << 7)   /* on battery */
#define MQ_BATTERY_LOW   (1u << 6)
#define MQ_BOOST_BUCK    (1u << 5)   /* bypass, boost or buck active */
#define MQ_UPS_FAILED    (1u << 4)
#define MQ_STANDBY_TYPE  (1u << 3)   /* 1 line-interactive/standby, 0 online */
#define MQ_TEST          (1u << 2)   /* self-test in progress */
#define MQ_SHUTDOWN      (1u << 1)   /* shutdown active */
#define MQ_BEEPER_ON     (1u << 0)

typedef struct {
    float input_voltage, input_fault_voltage, output_voltage;
    float load, input_frequency, battery_voltage, temperature;
    uint8_t bits;
} mq_q1_t;

typedef struct {
    float voltage, current, battery_voltage, frequency;     /* MQ_ABSENT if not given */
} mq_rating_t;

/* A Q1 reply, n bytes (need not be NUL-terminated; stops at '\r'). False for
 * anything that is not a Q1 line: no leading '(', fewer than seven fields,
 * status not eight 0/1 characters, or a line that is all zeros while saying
 * it is on mains -- a UPS answered in another dialect, as in NUT issue #1539,
 * where every value read 0. */
bool mq_parse_q1(const char *s, size_t n, mq_q1_t *out);

/* An F reply ('#' then four fields). False if it is not one. */
bool mq_parse_rating(const char *s, size_t n, mq_rating_t *out);

/* An I reply into mfr and model (trimmed). False if it is not one. */
bool mq_parse_info(const char *s, size_t n, char *mfr, size_t mfr_cap,
                   char *model, size_t model_cap);

/* Charge in percent from battery voltage, or MQ_ABSENT when it cannot be
 * estimated. nominal_battery is F's nominal battery voltage (MQ_ABSENT if the
 * UPS gave none: then 12 V per battery is assumed). The reply may be per cell
 * or per pack; the multiplier that puts it nearest nominal is used, as NUT
 * does. Linear from low = 104/120 to high = 130/120 of nominal, clamped. */
float mq_charge_estimate(float battery_voltage, float nominal_battery);

/* The 0001:0000 string-command dialects, tried in this order on a unit that
 * answers none of the HID usages: hunnox first (the one NUT issue #1539 found
 * working on the MEC0003), then fabula, then krauler. */
typedef enum {
    MQ_DIALECT_HUNNOX = 1,  /* preamble once, LANGID 0x0409, UTF-16 reply */
    MQ_DIALECT_FABULA,      /* plain string requests */
    MQ_DIALECT_KRAULER,     /* plain, different index for C, retries */
} mq_dialect_t;

/* The string-descriptor index that carries a command, for a dialect; 0 if the
 * dialect has no index for it. Only reading commands are listed: Q1, F, I. */
uint8_t mq_string_index(mq_dialect_t d, const char *cmd);

/* A Q1-speaking transport known for this USB ID. */
typedef enum {
    MQ_XPORT_NONE = 0,
    MQ_XPORT_STRING,        /* 0001:0000 family: the command is a string index */
    MQ_XPORT_CYPRESS,       /* HID SET_REPORT out, interrupt IN back */
} mq_xport_t;

mq_xport_t mq_transport_for(uint16_t vid, uint16_t pid);

/* Whether a parsed HID descriptor gives on-mains/battery status or charge.
 * A Q1 unit's (the MEC0003's) describes voltages only; a real HID UPS on a
 * shared ID always has these. */
bool mq_hid_gives_status(const hid_report_map_t *map);

/* The one rule for trying Q1: a known Q1 USB ID, AND a descriptor that gives
 * no status. A HID UPS is never sent Q1. */
bool mq_should_try(uint16_t vid, uint16_t pid, const hid_report_map_t *map);

#endif /* UPSA_MEGATEC_H */
