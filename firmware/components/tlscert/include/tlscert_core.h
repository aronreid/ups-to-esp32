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

/* A clock more than this far past the firmware's build is not believed: the
 * time comes from an unauthenticated NTP pool, and a spoofed far-future reply
 * would otherwise issue a certificate that is not valid until then. */
#define TLSC_CLOCK_MAX_AHEAD  (20u * 365u * 86400u)

/* The time a new certificate counts from: the clock if it is set and
 * plausible, otherwise the firmware's build time, which is at least never in
 * the future; never before TLSC_EPOCH_FLOOR. */
uint32_t tlsc_ref_time(uint32_t clock_epoch, uint32_t build_epoch);

/* A certificate re-issued only because the board's address or name changed
 * (not because it is near its end) waits at least this long since the last
 * one: a DHCP server that keeps changing the address must not be able to make
 * the board re-issue, write flash and restart HTTPS every ten minutes. */
#define TLSC_REISSUE_MIN_US  ((int64_t)3600 * 1000000)

/* Is the certificate within TLSC_RENEW_DAYS of its end at `now` (UTC)? */
bool tlsc_expiring(uint32_t not_after, uint32_t now);

/* May a certificate be issued now? last_issue_us is when the last one was
 * (0 = none yet this boot). Expiry is never made to wait. */
bool tlsc_may_reissue(int64_t last_issue_us, int64_t now_us, bool expiring);

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
