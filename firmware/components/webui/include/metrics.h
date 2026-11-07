/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Prometheus metrics, served at /metrics in the text exposition format.
 *
 * UPS readings use nut_exporter's names -- network_ups_tools_<NUT variable,
 * dots as underscores>, network_ups_tools_ups_status{flag=...}, and
 * network_ups_tools_device_info -- so dashboards and alerts written for NUT
 * through nut_exporter should work when Prometheus scrapes this board
 * directly. The board's own health is under ups_esp32_.
 *
 * A value the UPS does not report is left out, never exported as 0: the same
 * rule as NUT and /api/status. A missing series is honest; a zero battery
 * charge would page someone.
 *
 * Plain C with no ESP-IDF, so tools/test/test_metrics.py renders and parses it
 * on the host.
 */
#ifndef UPSA_METRICS_H
#define UPSA_METRICS_H

#include <stdbool.h>
#include <stdint.h>
#include "ups_data.h"

typedef struct {
    const ups_data_t *ups;
    const char *ups_name;       /* NUT's UPS name, the "ups" label */
    const char *board;          /* BOARD_NAME */
    const char *version;        /* firmware version */
    const char *hostname;
    uint64_t    uptime_s;
    uint32_t    heap_free;
    uint32_t    heap_min_free;
    int         wifi_rssi;      /* dBm; 0 when not associated, and then left out */
    int         nut_clients;
    uint32_t    crashes;
} metrics_src_t;

/* Called with each piece of the output in order; the handler streams them. */
typedef void (*metrics_write_fn)(void *ctx, const char *text);

void metrics_render(const metrics_src_t *m, metrics_write_fn write, void *ctx);

#endif /* UPSA_METRICS_H */
