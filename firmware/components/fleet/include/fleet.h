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
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     configured;     /* built with an endpoint and a key            */
    bool     enabled;        /* owner allows the heartbeat (default on)     */
    bool     stats;          /* owner opted in to the extra stats (off)     */
    /* A dev or debug board -- one of the project's own test boards, as the
     * fleet server last said. Stats and device diagnostics are then sent
     * whatever the stats box says; production boards send them only if the
     * owner opted in, and never the diagnostics. */
    bool     stats_auto;
    char     channel[12];    /* production, debug or dev, from the server   */
    uint32_t sent;           /* heartbeats answered this boot               */
    int64_t  last_ok_s;      /* uptime at the last answered one, -1 if none */
    char     last_error[48]; /* why the last attempt failed, "" if it didn't */
} fleet_status_t;

esp_err_t fleet_start(void);
void      fleet_get(fleet_status_t *out);
/* Persisted. Turning either on sends a heartbeat soon; turning stats off
 * sends one without them, which clears what the server kept. */
esp_err_t fleet_set(bool enabled, bool stats);
/* Check in within seconds rather than at the next interval, if allowed. The
 * page's "check now" does, so a board has its current tier and target -- a
 * change there triggers another update check by itself. */
void      fleet_checkin_soon(void);

/* "Send my UPS info": the UPS's report descriptor, model and USB id, on the
 * owner's request, whether or not stats are ticked. It also checks in once,
 * because the server takes a descriptor only from a board it knows. Returns
 * at once; fleet_descriptor_result() says how it went: "" never asked,
 * "sending", "sent", or "failed: ...". */
void      fleet_send_descriptor(void);
void      fleet_descriptor_result(char *out, size_t n);

/* Separately, with the check-in on, a board whose UPS enumerated but gives no
 * readings for five minutes sends that descriptor once, unasked: a model the
 * parser cannot read is exactly the case the project cannot fix blind. */

/* The device diagnostics a test board sends (chip, build, memory, address,
 * crash count, update state), as a cJSON object the caller must delete. The
 * page's Debug card shows the same, plus what only makes sense locally. */
struct cJSON;
struct cJSON *fleet_diag(void);

#endif /* UPSA_FLEET_H */
