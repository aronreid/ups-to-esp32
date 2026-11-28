/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Diagnostic logs for release-candidate boards.
 *
 * A board that offers release candidates (ota rc_opt), or that the fleet
 * server names a test board, keeps its recent log lines in a RAM ring and can
 * send them to the fleet server (fleet.c, RPC upload_log), so a person with a
 * problem does not have to dig logs out of a serial console. A board with
 * neither runs none of this: no hook, no ring, nothing sent. The gate is
 * checked at runtime and rechecked as the owner's setting or the tier changes,
 * so every build carries the code and a release candidate promoted to the
 * full release is the same tree that was tested.
 *
 * What is sent is scrubbed first (diaglog_core.h): never a password, key or
 * credential. Rules and the data kept: docs/privacy.md.
 */
#ifndef UPSA_DIAGLOG_H
#define UPSA_DIAGLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diaglog_core.h"

/* The most text one upload carries. */
#define DIAGLOG_UPLOAD_MAX  (16 * 1024)

/* Check the gate and turn capture on or off to match. Cheap; call after
 * ota_start() and from any periodic task (fleet_task does, every 10 s). */
void diaglog_poll(void);

/* Capture is on: the gate is open and the ring exists. */
bool diaglog_enabled(void);

/* Ask for an upload: reason is "crash", "usb_fail", "daily" or "asked".
 * Ignored when the gate is closed; "usb_fail" at most once per 10 minutes.
 * Safe from any task. The upload itself goes with the next check-in. */
void diaglog_request_upload(const char *reason);

/* Called by fleet to learn what, if anything, to send now. Returns the reason
 * (a string constant) and consumes the request, or NULL for none: nothing
 * asked, the gate closed, or 12 uploads already went in the last day. */
const char *diaglog_take_request(void);

/* The newest log text, scrubbed, oldest line first, NUL-terminated, at most
 * cap - 1 bytes. Returns its length. */
size_t diaglog_snapshot(char *out, size_t cap);

/* Capacity of the ring: what an upload buffer needs to hold. 0 if off. */
size_t diaglog_capacity(void);

/* An upload was tried (ok or not). Counts toward the daily limit, and when
 * ok records the time for the page. */
void diaglog_note_upload(bool ok);

/* Seconds since the last upload that went through, or -1 if none this boot. */
int32_t diaglog_last_upload_ago_s(void);

/* How diaglog asks fleet to check in soon, so a request does not wait hours.
 * fleet registers it; diaglog cannot depend on fleet. */
void diaglog_set_kick(void (*kick)(void));

#endif /* UPSA_DIAGLOG_H */
