/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NUT's ups.status, built the way usbhid-ups builds it (usbhid-ups.c:
 * process_boolean_info(), ups_status_set(), ups_alarm_set()), from the tokens
 * NUT's status lookups produce. Pure: tools/test/test_nutstatus.py runs it.
 *
 * Tokens persist between polls, as NUT's ups_status does: a lookup only sets
 * or clears the one it names. Defaults are NUT's: lbrb_log_delay_sec 0 (LB
 * and RB immediately), onlinedischarge_* off (OL+DISCHRG reports OL).
 */
#ifndef UPSA_UPS_NUTSTATUS_H
#define UPSA_UPS_NUTSTATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NUT's status_info[] order; tools/gen-vartab.py's TOKENS must match. */
enum {
    NST_ONLINE, NST_OFFLINE, NST_DISCHRG, NST_CHRG, NST_LOWBATT, NST_OVERLOAD,
    NST_REPLACEBATT, NST_SHUTDOWNIMM, NST_TRIM, NST_BOOST, NST_BYPASSAUTO,
    NST_BYPASSMAN, NST_ECOMODE, NST_ESSMODE, NST_OFF, NST_CAL, NST_OVERHEAT,
    NST_COMMFAULT, NST_DEPLETED, NST_TIMELIMITEXP, NST_FULLYCHARGED,
    NST_NOTFULLYCHARGED, NST_AWAITINGPOWER, NST_FANFAIL, NST_NOBATTERY,
    NST_BATTVOLTLO, NST_BATTVOLTHI, NST_CHARGERFAIL, NST_VRANGE, NST_FRANGE,
    NST_COUNT
};
#define NST(x) (1u << (x))

/* process_boolean_info(): set or clear one token, and neuter its opposite
 * where NUT does (online/offline, fullycharged/notfullycharged). */
void ups_nutstatus_apply(uint32_t *bits, int token, bool clear);

/* ups_status_set(): the ups.status words. `charge` is battery.charge, or -1
 * if unknown (CHRG then needs a FullyCharged usage). Empty if the UPS gives
 * no power state and is not discharging. */
void ups_nutstatus_string(uint32_t bits, int charge, char *out, size_t cap);

/* ups_alarm_set(): the ups.alarm text, "" for none. */
void ups_nutstatus_alarm(uint32_t bits, char *out, size_t cap);

/* input.transfer.reason, or NULL for none. */
const char *ups_nutstatus_transfer_reason(uint32_t bits);

#endif
