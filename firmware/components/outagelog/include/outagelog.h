/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The outage log: every transfer to battery, kept on the Rev B board's
 * `outagelog` partition across restarts and updates. What is
 * recorded and how it is laid out is in outagelog_core.h.
 *
 * Only built with CONFIG_UPSA_OUTAGE_LOG (Rev B: the N4 has no flash for
 * it). Callers guard every call with that symbol.
 *
 * Also sets the board's clock over SNTP, which nothing else on the board
 * needs: a record made before the clock is set carries its uptime, and gets
 * a time when served if the clock is set later in the same boot.
 */
#ifndef UPSA_OUTAGELOG_H
#define UPSA_OUTAGELOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

/* The outagelog partition (partitions-revb.csv) is shared: these outage
 * records in its first 8 MB, the long history (components/history) in the
 * rest, about 1.9 MB. FIXED FOR GOOD: the boards in the field keep their
 * records where these say, and moving the line would read one as the other. */
#define OUTAGELOG_PART_SUBTYPE  0x40
#define OUTAGELOG_PART_LABEL    "outagelog"
#define OUTAGELOG_LOG_BYTES     (8u * 1024 * 1024)

#if CONFIG_UPSA_OUTAGE_LOG

/* Map the partition, find the end of the log, index it and start watching.
 * Call after ups_hid_start(), whose readings and quiet spells it uses, and
 * after netmgr_start() (SNTP). The boot scan reads through the MMU mapping,
 * so it does not disturb USB. A board whose partition is missing or
 * unreadable carries on without a log. */
esp_err_t outagelog_start(void);

typedef struct {
    bool     ready;         /* the partition was found and indexed */
    bool     active;        /* on battery now */
    bool     clock;         /* the board's clock is set */
    uint32_t outages;       /* outages in the index */
    uint32_t records;       /* records written since the log was last cleared */
    uint32_t capacity;      /* records the partition holds */
    uint32_t write_errors;  /* records that could not be written, this boot */
} outagelog_status_t;

void outagelog_status(outagelog_status_t *out);

/* Streams JSON through `emit`, which takes NUL-terminated pieces:
 * the newest `max` outages, as /api/outages serves them. */
void outagelog_json_list(void (*emit)(void *ctx, const char *s), void *ctx, size_t max);

/* One outage's readings, oldest first, as /api/outages/samples serves them.
 * False (and nothing emitted) if there is no such outage. */
bool outagelog_json_samples(uint32_t outage, void (*emit)(void *ctx, const char *s), void *ctx);

/* Forget every outage recorded so far. Nothing is erased: the log only
 * stops showing records older than now. */
esp_err_t outagelog_clear(void);

/* UTC seconds now, or 0 if the board's clock has not been set. */
uint32_t outagelog_epoch_now(void);

/* This boot's number, counted in NVS by outagelog_start(); 0 before it. */
uint16_t outagelog_boot(void);

#endif /* CONFIG_UPSA_OUTAGE_LOG */

#endif /* UPSA_OUTAGELOG_H */
