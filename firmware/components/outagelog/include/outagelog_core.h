/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The outage log's pure part: record format, the ring's layout on flash, the
 * outage state machine and the in-memory index. No ESP-IDF here, so
 * tools/test/test_outagelog.py compiles and runs it on the host; outagelog.c
 * is the glue that feeds it readings and puts its records on flash.
 *
 * WHAT IS LOGGED. Every transfer to battery: a START record when the UPS
 * reports OB, a SAMPLE every OLOG_SAMPLE_US while it stays there, and an END
 * when it is back on mains or the UPS is gone. The samples are the point --
 * if the UPS runs flat and takes this board with it, the last one written is
 * how long the battery really lasted under that load, which the UPS's own
 * runtime estimate only guesses at.
 *
 * THE RING. Fixed 32-byte records, 128 to a 4 KB sector, written in order
 * across the partition and wrapping; the sector about to be reused is erased
 * whole, so the oldest 128 records go at once. Every record carries a
 * sequence number that only grows, so after a restart the newest record is
 * found by scanning, with nothing stored anywhere else. A record cut short by
 * a power cut fails its CRC and is skipped. Its 8 MB share of the partition
 * is 262,144 records: at one a half-minute on battery, about 90 days of
 * outages.
 */
#ifndef UPSA_OUTAGELOG_CORE_H
#define UPSA_OUTAGELOG_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OLOG_MAGIC        0x4C4Fu   /* "OL" */
#define OLOG_REC_SIZE     32u
#define OLOG_SECTOR       4096u
#define OLOG_PER_SECTOR   (OLOG_SECTOR / OLOG_REC_SIZE)

#define OLOG_NONE8        0xFFu     /* a reading the UPS does not report */
#define OLOG_NONE16       0xFFFFu

typedef enum {
    OLOG_START  = 1,
    OLOG_SAMPLE = 2,
    OLOG_END    = 3,
} olog_type_t;

/* Why an outage ended: the low three bits of an END record's flags. */
typedef enum {
    OLOG_END_OPEN       = 0,        /* not ended (never on a record)          */
    OLOG_END_MAINS      = 1,        /* the UPS said it was back on mains      */
    OLOG_END_UPS_LOST   = 2,        /* the UPS left the USB bus while on battery */
    OLOG_END_POWER_LOST = 3,        /* this board lost power during it: the battery
                                     * most likely ran out. Written at next boot. */
    OLOG_END_RESTART    = 4,        /* this board restarted during it (not a power
                                     * loss) and the UPS was on mains after. */
} olog_end_t;

#define OLOG_F_REASON   0x07u
#define OLOG_F_LOWBATT  0x08u       /* the UPS reported LB at some point in it   */
#define OLOG_F_TEST     0x10u       /* a self-test was running when it began     */
#define OLOG_F_RESUMED  0x20u       /* first record after a restart part-way in  */
#define OLOG_F_COPIED   0x40u       /* an END written after a restart: its time and
                                     * readings are the last ones recorded before */

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;          /* olog_type_t */
    uint8_t  flags;         /* OLOG_F_* */
    uint32_t seq;           /* record number; only ever grows */
    uint32_t outage;        /* which outage this belongs to */
    uint32_t epoch;         /* UTC seconds, 0 when the board's clock was not set */
    uint32_t uptime_s;      /* seconds into boot number `boot` */
    uint16_t boot;          /* boot count, from NVS */
    uint8_t  charge;        /* %, OLOG_NONE8 if not reported */
    uint8_t  load;          /* %, ditto */
    uint16_t runtime_s;     /* UPS's estimate, saturating at 65534 */
    uint16_t input_dv;      /* input volts x10 */
    uint16_t batt_cv;       /* battery volts x100 */
    uint16_t crc;           /* CRC-16/CCITT of the 30 bytes before it */
} olog_rec_t;

_Static_assert(sizeof(olog_rec_t) == OLOG_REC_SIZE, "a record is 32 bytes");

uint16_t olog_crc16(const void *data, size_t len);
void     olog_seal(olog_rec_t *r);                   /* sets magic and crc */
bool     olog_valid(const olog_rec_t *r);
bool     olog_erased(const void *p, size_t len);     /* every byte 0xFF */

/* Readings into record units. Negative (UPS_VALUE_ABSENT) is "not reported". */
uint8_t  olog_u8(float v);                           /* %, 0..100 */
uint16_t olog_x10(float v);
uint16_t olog_x100(float v);
uint16_t olog_secs(float v);

/* ---- the ring on flash ---- */

typedef struct {
    uint32_t next_slot;     /* where the next record goes */
    uint32_t next_seq;      /* the sequence number it gets */
    uint32_t first_sector;  /* oldest sector, where a walk in time order starts */
} olog_scan_t;

/* Find the end of the log in `nslots` records of mapped flash. */
void olog_scan(const uint8_t *base, uint32_t nslots, olog_scan_t *out);

/* ---- the state machine ---- */

typedef struct {
    bool  attached;         /* the UPS is enumerated and answering */
    bool  onbatt;           /* OB */
    bool  lowbatt;          /* LB */
    bool  testing;          /* a self-test is running */
    float charge, load, runtime, vin, vbat;   /* UPS_VALUE_ABSENT if not reported */
} olog_in_t;

#define OLOG_SAMPLE_US    (30LL * 1000000)  /* one sample a half-minute on battery */
#define OLOG_LOST_US      (120LL * 1000000) /* the UPS gone this long ends an outage */
#define OLOG_RESUME_US    (90LL * 1000000)  /* after a restart, wait this long for the UPS */

typedef struct {
    bool       active;      /* an outage is open */
    uint32_t   outage;      /* its number, or the last one's */
    int64_t    last_sample_us;
    int64_t    lost_since_us;
    uint8_t    flags;       /* LB and TEST, carried to every record */
    olog_rec_t last;        /* the last record written for the open outage */
    /* Set by olog_resume(): an outage was open when this board went down. */
    bool       resume;
    uint8_t    resume_reason;   /* OLOG_END_POWER_LOST or OLOG_END_RESTART */
} olog_sm_t;

/* Pick up an outage that was still open when the board went down. `last` is
 * the last record written for it; `power_lost` is whether this boot followed
 * a loss of power rather than a restart. */
void olog_resume(olog_sm_t *sm, const olog_rec_t *last, bool power_lost);

/* One reading, `now_us` into this boot. Writes 0, 1 or 2 records to `out`
 * and returns how many. Each has type, flags, outage, uptime_s and readings
 * filled; the caller adds seq, boot and epoch -- except on an OLOG_F_COPIED
 * END, whose boot, uptime and epoch are the last record's and stay as given
 * -- then seals and appends it. */
int olog_step(olog_sm_t *sm, const olog_in_t *in, int64_t now_us, olog_rec_t out[2]);

/* ---- the index ---- */

typedef struct {
    uint32_t outage;
    uint32_t first_slot;    /* where its first record is */
    uint32_t start_epoch, end_epoch;   /* 0 unknown */
    uint32_t start_up, end_up;         /* uptime of first and last record */
    uint16_t start_boot, end_boot;
    uint32_t dur_s;         /* on battery, summed across the boots it spans */
    bool     gap;           /* spans a restart of unknown length: dur_s is short */
    uint8_t  charge0, charge1, charge_min, load_max;
    uint16_t rt0, rt1, vin0;
    uint8_t  flags;         /* every record's flags ORed, less the reason */
    uint8_t  reason;        /* olog_end_t, OLOG_END_OPEN while open */
    uint16_t records;
    /* the latest record's time, for summing */
    uint32_t last_epoch, last_up;
    uint16_t last_boot;
} olog_outage_t;

typedef struct {
    olog_outage_t *v;       /* a ring of cap entries, newest last */
    size_t   cap, n, head;
    uint32_t total;         /* outages ever indexed, including ones evicted */
    uint32_t min_seq;       /* records below this are forgotten (cleared) */
} olog_index_t;

void olog_index_init(olog_index_t *ix, olog_outage_t *store, size_t cap, uint32_t min_seq);
void olog_index_add(olog_index_t *ix, const olog_rec_t *r, uint32_t slot);
/* i = 0 is the newest. NULL past the end. */
const olog_outage_t *olog_index_get(const olog_index_t *ix, size_t i);
const olog_outage_t *olog_index_find(const olog_index_t *ix, uint32_t outage);

#endif /* UPSA_OUTAGELOG_CORE_H */
