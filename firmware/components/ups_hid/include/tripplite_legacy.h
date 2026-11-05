/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tripp Lite's legacy USB protocol (product ID 0x0001).
 *
 * Most Tripp Lite units are standard HID Power Devices and take the generic
 * path. Older ones -- OMNIVS, SMARTPRO, some SMART and INTERNETOFFICE revisions,
 * all enumerating as 09ae:0001 -- speak a vendor protocol instead: short ASCII
 * commands sent with HID SET_REPORT, answered on the interrupt IN endpoint. The
 * report descriptor is vendor-defined, so the generic parser finds nothing.
 *
 * This is the decoding half: packets and replies, nothing that touches USB, so
 * tools/test/test_tripplite_legacy.py compiles and runs it on the host. The
 * transport is in ups_hid.c.
 *
 * Ported from Network UPS Tools' drivers/tripplite_usb.c, GPL-2.0-or-later:
 *   Copyright (C) 1999  Russell Kroll <rkroll@exploits.org>
 *   Copyright (C) 2001  Rickard E. (Rik) Faith <faith@alephnull.com>
 *   Copyright (C) 2004  Nicholas J. Kain <nicholas@kain.us>
 *   Copyright (C) 2005-2008, 2014  Charles Lepple <clepple+nut@gmail.com>
 *   Copyright (C) 2016  Eaton
 *   Copyright (C) 2023  Eliran Sapir <e@vcboy.com>
 * The decoding below follows that driver's upsdrv_initinfo, upsdrv_updateinfo
 * and decode_v line for line, including its guesses; where NUT is unsure, so is
 * this, and the comments say so.
 */
#ifndef UPSA_TRIPPLITE_LEGACY_H
#define UPSA_TRIPPLITE_LEGACY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TL_VID          0x09AE
#define TL_LEGACY_PID   0x0001
#define TL_PKT          8       /* every command and reply is 8 bytes */

typedef enum {
    TL_UNKNOWN = 0,
    TL_OMNIVS,          /* protocol 1001 */
    TL_OMNIVS_2001,     /* 2001 */
    TL_SMARTPRO,        /* 3003 */
    TL_SMART_0004,      /* 0004, older SMART */
    TL_SMART_3005,      /* 3005, values in BINARY rather than hex text */
    TL_SMART_3017,      /* 3017, (mostly) ASCII SMART */
} tl_model_t;

tl_model_t  tl_decode_protocol(uint16_t proto);
const char *tl_model_name(tl_model_t m);
bool        tl_is_smart(tl_model_t m);
bool        tl_is_binary(tl_model_t m);

/* Build the 8-byte packet for a command, exactly as NUT's send_cmd does.
 *
 * `msg` and `len` follow NUT's convention of passing sizeof() a string literal,
 * so len INCLUDES the trailing NUL: "S" is sent as tl_build_cmd("S", 2). The
 * checksum (255 minus the byte sum) then lands on that NUL's position, and a
 * carriage return follows. Packet: ':' cmd [args] checksum '\r' [pad NUL].
 * len must be 1..5. */
void tl_build_cmd(const uint8_t *msg, size_t len, uint8_t out[TL_PKT]);

/* A reply belongs to a command if it starts with that command's letter. An
 * unsupported command, or an internal serial timeout, makes the UPS echo its
 * PREVIOUS reply instead, which this rejects. */
bool tl_reply_is_for(const uint8_t cmd_pkt[TL_PKT], const uint8_t reply[TL_PKT]);

/* Protocol number from the reply to the "\0" command. */
uint16_t tl_protocol_from_reply(const uint8_t reply[TL_PKT]);

/* What the "V" command says about the unit. Defaults per NUT: 12 V battery,
 * 120 V input. */
typedef struct {
    tl_model_t model;
    int  battery_nominal;       /* V */
    int  input_nominal;         /* V */
    int  input_scaled;          /* V, the scale the raw readings are relative to */
    int  load_banks;            /* switchable load banks, -1 unknown */
} tl_config_t;

void tl_config_init(tl_config_t *c, tl_model_t model);
void tl_decode_v(tl_config_t *c, const uint8_t v[TL_PKT]);

/* One poll's readings. TL_ABSENT where this protocol does not report it. */
#define TL_ABSENT (-1.0f)
#define TL_ST_OL      (1u << 0)
#define TL_ST_OB      (1u << 1)
#define TL_ST_LB      (1u << 2)
#define TL_ST_RB      (1u << 3)
#define TL_ST_OVER    (1u << 4)
#define TL_ST_OFF     (1u << 5)   /* output off */
#define TL_ST_BYPASS  (1u << 6)   /* OMNIVS "charge-only" mode */
#define TL_ST_CAL     (1u << 7)   /* calibrating */

typedef struct {
    uint32_t status;            /* TL_ST_* */
    bool     stale;             /* a value NUT would call unknown: distrust the poll */
    float    battery_v12;       /* battery voltage relative to 12 V, for the charge estimate */
    float    battery_voltage;
    float    battery_charge;    /* %, ESTIMATED from voltage: these units do not report it */
    float    input_voltage;
    float    input_frequency;
    float    output_voltage;
    float    load;              /* % */
    float    temperature;       /* C, NUT calls its own formula a guess */
} tl_reading_t;

void tl_reading_init(tl_reading_t *r);

/* Each decodes one reply into r. They never fail: an unrecognised status code
 * sets r->stale, as NUT marks the data stale. */
void tl_decode_s(const tl_config_t *c, const uint8_t s[TL_PKT], tl_reading_t *r);
void tl_decode_b(const tl_config_t *c, const uint8_t b[TL_PKT], tl_reading_t *r);   /* OMNIVS */
void tl_decode_d(const tl_config_t *c, const uint8_t d[TL_PKT], tl_reading_t *r);   /* SMART */
void tl_decode_t(const tl_config_t *c, const uint8_t t[TL_PKT], tl_reading_t *r);   /* SMART */
void tl_decode_l(const tl_config_t *c, const uint8_t l[TL_PKT], tl_reading_t *r);

/* Battery charge from battery_v12, NUT's continuous fit: 100% at 13.4 V,
 * 10% at 11.0 V and below, sqrt-shaped between. Call after B or D. */
void tl_estimate_charge(tl_reading_t *r);

/* Which commands a poll sends for this protocol, in order: "S" always, then
 * "B" (OMNIVS) or "D" and "T" (SMART), then "L". Writes up to 4 letters. */
size_t tl_poll_commands(tl_model_t m, char out[4]);

#endif /* UPSA_TRIPPLITE_LEGACY_H */
