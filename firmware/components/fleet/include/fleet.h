/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fleet heartbeat: a few times a day the board tells the project's database
 * that it is alive and which firmware it runs, and the reply says which update
 * channel it follows. With the owner's opt-in it also sends anonymous stats
 * about the UPS and the board's health. See telemetry/supabase.sql for exactly
 * what is kept, and README "Fleet reporting" for what the owner is told.
 *
 * NOTHING HERE MAY GET IN THE WAY OF THE BOARD'S JOB. It runs in its own task
 * at the lowest application priority, never subscribes to the task watchdog,
 * skips a round rather than wait for memory or for the TLS gate, gives up on a
 * slow server within seconds, and backs off when the server is away. A board
 * that never reaches the server behaves exactly like one built without this.
 */
#ifndef UPSA_FLEET_H
#define UPSA_FLEET_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     configured;     /* built with an endpoint and a key            */
    bool     enabled;        /* owner allows the heartbeat (default on)     */
    bool     stats;          /* owner opted in to the extra stats (off)     */
    bool     dev;            /* the server tags this board a dev board      */
    uint32_t sent;           /* heartbeats answered this boot               */
    int64_t  last_ok_s;      /* uptime at the last answered one, -1 if none */
    char     last_error[48]; /* why the last attempt failed, "" if it didn't */
} fleet_status_t;

esp_err_t fleet_start(void);
void      fleet_get(fleet_status_t *out);
/* Persisted. Turning either on sends a heartbeat soon; turning stats off
 * sends one without them, which clears what the server kept. */
esp_err_t fleet_set(bool enabled, bool stats);

#endif /* UPSA_FLEET_H */
