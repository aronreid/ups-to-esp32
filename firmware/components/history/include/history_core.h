/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Long history's pure part: how readings are laid out on flash and how they
 * are boiled down for a chart. No ESP-IDF here; tools/test/test_history.py
 * compiles and runs it on the host. history.c is the glue.
 *
 * ONE READING EVERY HIST_PERIOD_S, whatever the UPS is doing, in 4 KB
 * sectors: a 32-byte header, then 508 readings of 8 bytes, each sector 85
 * minutes. The header says when the sector began -- its sequence number,
 * boot, uptime and, if the clock was set, UTC time -- and the readings
 * follow at a fixed period, so no reading carries a time of its own. A
 * restart, or the clock being set, starts a new sector. The ring of 474
 * sectors in the partition's last 1.9 MB is about 28 days.
 *
 * Flash is written once per erase, so readings go into an erased sector one
 * batch at a time and are never rewritten. An unwritten slot reads all 0xFF,
 * and HIST_F_UNWRITTEN (bit 7 of flags, always clear in a written one) tells
 * the two apart even when every reading in a slot is absent.
 */
#ifndef UPSA_HISTORY_CORE_H
#define UPSA_HISTORY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIST_MAGIC        0x54534948u     /* "HIST" */
#define HIST_SECTOR       4096u
#define HIST_PERIOD_S     10u
#define HIST_NONE8        0xFFu
#define HIST_NONE16       0xFFFFu

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;           /* sector number; only ever grows */
    uint32_t epoch;         /* UTC of the first reading, 0 if the clock was not set */
    uint32_t uptime_s;      /* of the first reading, into boot `boot` */
    uint16_t boot;
    uint16_t period_s;
    uint8_t  reserved[10];
    uint16_t crc;           /* CRC-16/CCITT of the 30 bytes before it */
} hist_hdr_t;

/* Status bits. Bit 7 is never set in a written reading. */
#define HIST_F_PRESENT    0x01u   /* the UPS was answering; the readings mean something */
#define HIST_F_OL         0x02u
#define HIST_F_OB         0x04u
#define HIST_F_LB         0x08u
#define HIST_F_CHRG       0x10u
#define HIST_F_UNWRITTEN  0x80u

typedef struct __attribute__((packed)) {
    uint8_t  charge;        /* %, HIST_NONE8 absent */
    uint8_t  load;          /* %, ditto */
    uint16_t vin_dv;        /* input volts x10, HIST_NONE16 absent */
    uint16_t runtime_10s;   /* the UPS's estimate, tens of seconds */
    uint8_t  flags;         /* HIST_F_* */
    uint8_t  watts_10;      /* output power, tens of watts, HIST_NONE8 absent */
} hist_smp_t;

_Static_assert(sizeof(hist_hdr_t) == 32, "the header is 32 bytes");
_Static_assert(sizeof(hist_smp_t) == 8, "a reading is 8 bytes");

#define HIST_PER_SECTOR   ((HIST_SECTOR - sizeof(hist_hdr_t)) / sizeof(hist_smp_t))   /* 508 */

uint16_t hist_crc16(const void *data, size_t len);
void     hist_seal(hist_hdr_t *h);
bool     hist_hdr_valid(const hist_hdr_t *h);
bool     hist_smp_written(const hist_smp_t *s);

/* Readings into a stored one. Negative values are "not reported". */
typedef struct {
    bool  present, ol, ob, lb, chrg;
    float charge, load, vin, runtime, watts;
} hist_in_t;
hist_smp_t hist_encode(const hist_in_t *in);

/* How many readings a mapped sector holds: the first unwritten slot. */
uint32_t hist_count(const uint8_t *sector);

/* The sector with the highest sequence number in `nsec` mapped sectors, or
 * -1 if none has a valid header. */
int32_t hist_newest(const uint8_t *base, uint32_t nsec, uint32_t *seq);

/* ---- a chart's worth ---- */

typedef struct {
    uint32_t n;                         /* readings that fell in it */
    uint32_t charge_sum, load_sum, watts_sum;
    uint16_t charge_n, load_n, watts_n;
    uint16_t vin_min, vin_max;          /* HIST_NONE16 when none */
    uint8_t  flags_or;                  /* OB anywhere in it, say */
} hist_bucket_t;

typedef struct {
    uint32_t t0;                        /* UTC of the first bucket's start */
    uint32_t step;                      /* seconds per bucket */
    uint32_t nb;
    hist_bucket_t *b;
} hist_agg_t;

void hist_agg_init(hist_agg_t *a, hist_bucket_t *store, uint32_t nb, uint32_t t0, uint32_t step);
/* A reading made at `epoch`. Outside the window it is ignored. */
void hist_agg_add(hist_agg_t *a, uint32_t epoch, const hist_smp_t *s);

#endif /* UPSA_HISTORY_CORE_H */
