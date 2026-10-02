/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APC Modbus over USB: the readings an APC Smart-UPS keeps out of its HID
 * interface.
 *
 * Some Smart-UPS units on 051d:0003 (a Smart-UPS C 1500 on firmware UPS 15.1
 * among them) report only charge, runtime and status over HID. Their input
 * and output voltage, load, current and frequency are available only through
 * APC's Modbus register map. NUT reads that with its apc_modbus driver, and on
 * USB the transport is a pair of vendor reports on the same HID interface:
 * usage page 0xFF86, usage 0xFC for an Output report carrying a request to the
 * UPS, usage 0xFD for an Input report carrying its reply. Each report is the
 * report ID and 63 bytes of Modbus RTU frame, zero-padded, with no CRC (USB
 * already checks the bytes). A reply longer than 63 bytes continues in the
 * next report.
 *
 * The UPS answers only with Modbus turned on in its front-panel menu
 * (Configuration, Modbus, Enable). The vendor reports are in the descriptor
 * either way, so their presence says what to try, not that it will work.
 *
 * This is the protocol half: finding the reports, building a request,
 * reassembling a reply and decoding registers. Nothing touches USB, so
 * tools/test/test_apc_modbus.py compiles and runs it on the host. The
 * transport is in ups_hid.c.
 *
 * The register map and scaling are NUT's drivers/apc_modbus.c (GPL-2.0-or-
 * later, Copyright (C) 2023 Axel Gembe <axel@gembe.net>), and the USB framing
 * is the rtu_usb backend of the libmodbus fork it builds against (LGPL-2.1-or-
 * later, same author).
 */
#ifndef UPSA_APC_MODBUS_H
#define UPSA_APC_MODBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APCMB_VID           0x051D
#define APCMB_REPORT        64      /* report ID + 63 bytes of frame */
#define APCMB_PAYLOAD       63
#define APCMB_SLAVE         1       /* NUT's default slave id */
#define APCMB_MAX_REGS      64

/* Blocks this firmware reads, from NUT's register maps. */
#define APCMB_NOM_ADDR      588     /* ups.power.nominal, ups.realpower.nominal */
#define APCMB_NOM_COUNT     2
#define APCMB_DYN_ADDR      128     /* NUT's "dynamic" block: 128..171 */
#define APCMB_DYN_COUNT     44

/* The two vendor report IDs in a report descriptor: `to_ups` carries a
 * request (Output, usage 0xFF86:0xFC), `from_ups` a reply (Input, usage
 * 0xFF86:0xFD). False unless both are there. */
bool apcmb_find_reports(const uint8_t *desc, size_t len, uint8_t *to_ups, uint8_t *from_ups);

/* A Read Holding Registers (function 3) request for `count` registers from
 * `addr`, as one wire report. */
void apcmb_build_read(uint8_t report_id, uint8_t slave, uint16_t addr, uint16_t count,
                      uint8_t out[APCMB_REPORT]);

/* A reply being reassembled from reports. */
typedef struct {
    uint8_t  slave;
    uint16_t count;                 /* registers asked for */
    uint8_t  frame[3 + 2 * APCMB_MAX_REGS];
    size_t   len;
    uint8_t  exception;             /* the code, after APCMB_EXCEPTION */
} apcmb_rx_t;

typedef enum {
    APCMB_MORE = 0,     /* the frame continues in the next report */
    APCMB_DONE,         /* regs[] holds the registers */
    APCMB_EXCEPTION,    /* the UPS refused the read: rx->exception */
    APCMB_FOREIGN,      /* not a reply to this request (a late one to an
                         * earlier request, say): dropped, keep waiting */
} apcmb_rx_status_t;

void apcmb_rx_init(apcmb_rx_t *rx, uint8_t slave, uint16_t count);

/* One reply report's 63 bytes, the report ID already stripped. */
apcmb_rx_status_t apcmb_rx_feed(apcmb_rx_t *rx, const uint8_t *payload, size_t n,
                                uint16_t *regs);

/* The values NUT's apc_modbus publishes from the two blocks above. NAN-free:
 * APCMB_ABSENT where there is no value. */
#define APCMB_ABSENT (-1e30)
typedef struct {
    double power_nominal;           /* VA */
    double realpower_nominal;       /* W */
    double battery_voltage;
    double battery_temperature;
    double load;                    /* % */
    double realpower;               /* W, load x realpower_nominal */
    double power;                   /* VA */
    double output_current;
    double output_voltage;
    double output_frequency;
    double input_voltage;
    double efficiency;              /* %, or APCMB_ABSENT with efficiency_word */
    const char *efficiency_word;    /* "OnBattery" etc., NULL for a number */
} apcmb_reading_t;

void apcmb_reading_init(apcmb_reading_t *r);
void apcmb_decode_nominal(const uint16_t regs[APCMB_NOM_COUNT], apcmb_reading_t *r);
void apcmb_decode_dynamic(const uint16_t regs[APCMB_DYN_COUNT], apcmb_reading_t *r);

/* A reading as NUT variables: name and value text, in NUT's formats. */
typedef struct { const char *name; char value[24]; } apcmb_var_t;
#define APCMB_MAX_VARS 13
size_t apcmb_vars(const apcmb_reading_t *r, apcmb_var_t out[APCMB_MAX_VARS]);

#endif /* UPSA_APC_MODBUS_H */
