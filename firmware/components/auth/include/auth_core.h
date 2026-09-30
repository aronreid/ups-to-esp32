/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* The decisions behind the optional login, with no ESP-IDF in them, so
 * tools/test/test_auth.py compiles and runs them on the host. Storage, the
 * hash and the clock are the caller's (auth.c); nothing here allocates. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_USER_MAX   32
#define AUTH_PASS_MIN   8
#define AUTH_PASS_MAX   64
#define AUTH_SALT_LEN   16
#define AUTH_HASH_LEN   32
#define AUTH_TOKEN_LEN  32          /* hex characters */
#define AUTH_TOKENS     4           /* browsers logged in at once */

/* A token unused this long is forgotten: a tab left open on a laptop does not
 * stay logged in for ever. */
#define AUTH_TOKEN_IDLE_US   (12LL * 3600 * 1000000)
/* Wrong passwords in a row before checks are refused for a while. Shared by
 * the web login and NUT's PASSWORD, so neither is a way round the other. */
#define AUTH_MAX_FAILS       5
#define AUTH_LOCKOUT_US      (60LL * 1000000)

typedef void (*authc_hash_fn)(const uint8_t salt[AUTH_SALT_LEN], const char *pass,
                              uint8_t out[AUTH_HASH_LEN]);

typedef struct {
    char user[AUTH_USER_MAX + 1];   /* "" = login off: the board is open */
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t hash[AUTH_HASH_LEN];
    struct { char tok[AUTH_TOKEN_LEN + 1]; int64_t last_us; } tokens[AUTH_TOKENS];
    int fails;
    int64_t locked_until_us;
} authc_t;

typedef enum { AUTHC_OK, AUTHC_BAD, AUTHC_LOCKED } authc_result_t;

/* What NUT answers an INSTCMD or SET VAR with, in upsd's own words. */
typedef enum {
    AUTHC_NUT_ALLOW,
    AUTHC_NUT_USERNAME_REQUIRED,    /* ERR USERNAME-REQUIRED */
    AUTHC_NUT_PASSWORD_REQUIRED,    /* ERR PASSWORD-REQUIRED */
    AUTHC_NUT_DENIED,               /* ERR ACCESS-DENIED */
} authc_nut_t;

bool authc_enabled(const authc_t *a);

/* A username and password this board will store: printable ASCII with no
 * spaces or quotes, because NUT's USERNAME and PASSWORD are single words on a
 * text line. */
bool authc_shape_ok(const char *user, const char *pass);

/* Replace the credentials (hash computed by the caller) and log everyone out.
 * user "" turns the login off. */
void authc_set(authc_t *a, const char *user, const uint8_t salt[AUTH_SALT_LEN],
               const uint8_t hash[AUTH_HASH_LEN]);

authc_result_t authc_check(authc_t *a, const char *user, const char *pass,
                           int64_t now_us, authc_hash_fn hash);

/* Remember a token the caller generated; the oldest idle one makes room. */
void authc_issue(authc_t *a, const char *tok, int64_t now_us);
bool authc_token_ok(authc_t *a, const char *tok, int64_t now_us);
void authc_revoke(authc_t *a, const char *tok);

/* user and pass are what the client sent with USERNAME and PASSWORD on this
 * connection, NULL if it sent none. */
authc_nut_t authc_nut_write(authc_t *a, const char *user, const char *pass,
                            int64_t now_us, authc_hash_fn hash);

/* A browser sends Origin on a cross-site POST; a script like curl sends none.
 * Allowed: no Origin at all, or one naming the host the request was sent to.
 * This is what stops a web page open anywhere on the LAN from posting
 * load.off to the board, and it holds whether or not a login is set. */
bool authc_origin_ok(const char *origin, const char *host);
