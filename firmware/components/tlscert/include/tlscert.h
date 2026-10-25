/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The board's own certificates, for HTTPS on the page and STARTTLS on NUT.
 *
 * Two of them, both made on the board and kept in NVS (namespace "tls"):
 *
 *  - a CA, "<hostname> CA", valid 2026 to 2046. This is the one a person
 *    trusts, once, if they want the browser warning gone; the page offers it
 *    for download, with its SHA-256 to check it by.
 *  - a server certificate signed by that CA for <hostname>, <hostname>.local
 *    and the board's address, valid 800 days (Apple's platforms refuse more
 *    than 825 even with the CA trusted). The board issues a new one, with the
 *    same key, when its name or address changes or the end is 60 days away,
 *    so a trusted CA stays trusted.
 *
 * P-256 keys, generated with the hardware RNG; the private keys never leave
 * the board. Only built with CONFIG_UPSA_TLS (Rev B); callers guard every
 * call with that symbol.
 */
#ifndef UPSA_TLSCERT_H
#define UPSA_TLSCERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_UPSA_TLS

/* Load both from NVS, making whatever is missing, then issue a server
 * certificate if the one in hand no longer fits. About a second the first
 * time, and it writes NVS: call it from a task whose stack is in internal RAM,
 * about 6 KB of it. */
esp_err_t tlscert_load(void);

/* Cheap: whether the server certificate no longer fits the board's name,
 * address or clock. From any task. */
bool tlscert_stale(void);

/* Issue a new server certificate if tlscert_stale(). true if it did. Same
 * stack rules as tlscert_load(). */
bool tlscert_refresh(void);

/* The server certificate and its key, DER, for esp-tls. false before
 * tlscert_load() has succeeded. Buffers stay valid until the
 * certificate's replacement: a renewal makes new ones and leaves these in
 * place until the renewal after it, an hour or more later, so a TLS session
 * that is being set up while it happens is not left reading freed memory.
 * (An address change re-issues at most once an hour; otherwise a renewal comes
 * every two years.) */
bool tlscert_get(const uint8_t **crt, size_t *crt_len,
                 const uint8_t **key, size_t *key_len);

/* The CA as PEM, NUL-terminated; NULL before tlscert_load(). */
const char *tlscert_ca_pem(void);

/* SHA-256 of the CA, "AB:CD:...", as a browser's certificate viewer shows
 * it; "" before tlscert_load(). */
const char *tlscert_ca_sha256(void);

/* The server certificate's end, UTC seconds; 0 before tlscert_load(). */
uint32_t tlscert_not_after(void);

#endif /* CONFIG_UPSA_TLS */

#endif /* UPSA_TLSCERT_H */
