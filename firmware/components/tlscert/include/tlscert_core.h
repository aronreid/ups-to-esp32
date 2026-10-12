/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The certificate's pure part: dates and the decision to issue a new server
 * certificate. No ESP-IDF, so tools/test/test_tlscert.py runs it on the host.
 */
#ifndef UPSA_TLSCERT_CORE_H
#define UPSA_TLSCERT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The server certificate's life. Apple's platforms refuse a TLS server
 * certificate valid for more than 825 days even when its CA is trusted by
 * hand, so this stays under that; the board renews it well before the end. */
#define TLSC_LEAF_DAYS     800
#define TLSC_RENEW_DAYS    60
/* The CA's: the one thing a person installs, so it should outlast the board.
 * Ends before 2050, where X.509 switches from UTCTime to GeneralizedTime. */
#define TLSC_CA_NOT_BEFORE "20260101000000"
#define TLSC_CA_NOT_AFTER  "20460101000000"

/* 2026-01-01: a clock reading before this has not been set, and a time to
 * count from is never earlier. */
#define TLSC_EPOCH_FLOOR   1767225600u

/* esp_app_desc_t's __DATE__ and __TIME__ ("Oct 10 2026", "14:03:59") as UTC
 * seconds; 0 if they do not parse. */
uint32_t tlsc_build_epoch(const char *date, const char *time);

/* UTC seconds as X.509's "YYYYMMDDhhmmss". out needs 15 bytes. */
void tlsc_x509_time(uint32_t epoch, char out[15]);

/* The time a new certificate counts from: the clock if it is set, otherwise
 * the firmware's build time, which is at least never in the future; never
 * before TLSC_EPOCH_FLOOR. */
uint32_t tlsc_ref_time(uint32_t clock_epoch, uint32_t build_epoch);

/* Whether the server certificate in hand should be replaced. `names` is what
 * it was issued for ("host|ip"), `want` what the board is now; not_after is
 * its end. A missing one (names NULL) is always replaced. An address of
 * 0.0.0.0 (not connected yet) is not a change. */
bool tlsc_need_leaf(const char *names, uint32_t not_after,
                    const char *want_host, const char *want_ip, uint32_t now);

/* "host|ip" into out. */
void tlsc_names(char *out, size_t cap, const char *host, const char *ip);

/* SHA-256 as "AB:CD:...", 95 characters and a NUL. */
void tlsc_hex_colons(const uint8_t sha[32], char out[96]);

#endif /* UPSA_TLSCERT_CORE_H */
