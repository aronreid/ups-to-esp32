/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Long history: a reading of the UPS every ten seconds, kept on flash for
 * about 28 days across restarts and updates, for the page's chart. The
 * format is history_core.h's; it lives in the outagelog partition beyond
 * OUTAGELOG_LOG_BYTES.
 *
 * Only built with CONFIG_UPSA_HISTORY (Rev B). Callers guard every call
 * with that symbol.
 */
#ifndef UPSA_HISTORY_H
#define UPSA_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_UPSA_HISTORY

/* Find the newest sector and start sampling. After outagelog_start(), whose
 * boot number and clock it uses. */
esp_err_t history_start(void);

/* The last `span` seconds in at most `points` buckets, as JSON, through
 * `emit`. Times are seconds before now; without a clock, only this boot's
 * readings can be placed, by uptime. */
void history_json(uint32_t span, uint32_t points, void (*emit)(void *ctx, const char *s), void *ctx);

typedef struct {
    bool     ready;
    uint32_t sectors;       /* in the ring */
    uint32_t days;          /* that it holds, full */
    uint32_t write_errors;
} history_status_t;
void history_status(history_status_t *out);

#endif /* CONFIG_UPSA_HISTORY */

#endif /* UPSA_HISTORY_H */
