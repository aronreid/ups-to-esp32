/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNMP v2c: a read-only agent on UDP 161 (MIB-2 system group and the RFC 1628
 * UPS-MIB) and traps to one receiver. Both are off until the owner turns them
 * on and sets a community on the page; there is no default "public".
 *
 * Nothing is answered or sent while the board is on its own setup network,
 * which is open: the same rule as NUT and /metrics.
 */
#ifndef UPSA_SNMP_H
#define UPSA_SNMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "snmp_core.h"

/* Read the settings and, if either half is on, start the task. Never fatal:
 * a board without SNMP still serves NUT. */
esp_err_t snmp_start(void);

/* The settings as saved. The community is included: callers that show
 * settings (the page) must not send it back out. */
void snmp_get_cfg(snmp_cfg_t *out);

/* Check, save and apply. On a refusal ESP_ERR_INVALID_ARG with a sentence in
 * `why`. Starts the task the first time either half is turned on. */
esp_err_t snmp_set_cfg(const snmp_cfg_t *c, char *why, size_t why_len);

/* Send a warmStart trap to the saved receiver now, so the owner can see one
 * arrive. ESP_ERR_INVALID_STATE (with `why`) if traps are not set up or the
 * board is not on its network. UDP: "sent" is all the board can know. */
esp_err_t snmp_send_test(char *why, size_t why_len);

typedef struct {
    snmp_stats_t agent;
    uint32_t     traps_sent;
    uint32_t     trap_errors;
    uint32_t     rate_limited;
    bool         listening;      /* the agent's socket is open */
} snmp_status_t;

void snmp_get_status(snmp_status_t *out);

#endif /* UPSA_SNMP_H */
