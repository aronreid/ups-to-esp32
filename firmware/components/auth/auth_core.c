/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth_core.h"
#include <string.h>

/* Compare without an early exit, so the time taken does not say how many
 * leading bytes were right. */
static bool same_bytes(const void *x, const void *y, size_t n)
{
    const uint8_t *p = x, *q = y;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= p[i] ^ q[i];
    return d == 0;
}

static bool same_str(const char *x, const char *y, size_t cap)
{
    size_t nx = strnlen(x, cap + 1), ny = strnlen(y, cap + 1);
    if (nx != ny || nx > cap) return false;
    return same_bytes(x, y, nx);
}

bool authc_enabled(const authc_t *a) { return a->user[0] != '\0'; }

static bool word_ok(const char *s, size_t min, size_t max)
{
    if (!s) return false;
    size_t n = strnlen(s, max + 1);
    if (n < min || n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= ' ' || c > '~' || c == '"' || c == '\\') return false;
    }
    return true;
}

bool authc_shape_ok(const char *user, const char *pass)
{
    return word_ok(user, 1, AUTH_USER_MAX) && word_ok(pass, AUTH_PASS_MIN, AUTH_PASS_MAX);
}

void authc_set(authc_t *a, const char *user, const uint8_t salt[AUTH_SALT_LEN],
               const uint8_t hash[AUTH_HASH_LEN])
{
    memset(a, 0, sizeof(*a));
    if (user && user[0]) {
        strncpy(a->user, user, AUTH_USER_MAX);
        memcpy(a->salt, salt, AUTH_SALT_LEN);
        memcpy(a->hash, hash, AUTH_HASH_LEN);
    }
}

authc_result_t authc_check(authc_t *a, const char *user, const char *pass,
                           int64_t now_us, authc_hash_fn hash)
{
    if (!authc_enabled(a)) return AUTHC_BAD;
    if (now_us < a->locked_until_us) return AUTHC_LOCKED;
    bool ok = false;
    if (user && pass && strnlen(pass, AUTH_PASS_MAX + 1) <= AUTH_PASS_MAX) {
        uint8_t h[AUTH_HASH_LEN];
        hash(a->salt, pass, h);          /* always hashed, right user or not */
        /* Both compared every time, so a right username is not faster. */
        bool pass_ok = same_bytes(h, a->hash, AUTH_HASH_LEN);
        bool user_ok = same_str(user, a->user, AUTH_USER_MAX);
        ok = pass_ok && user_ok;
    }
    if (ok) {
        a->fails = 0;
        return AUTHC_OK;
    }
    if (++a->fails >= AUTH_MAX_FAILS) {
        a->fails = 0;
        a->locked_until_us = now_us + AUTH_LOCKOUT_US;
    }
    return AUTHC_BAD;
}

void authc_issue(authc_t *a, const char *tok, int64_t now_us)
{
    int slot = 0;
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (!a->tokens[i].tok[0]) { slot = i; break; }
        if (a->tokens[i].last_us < a->tokens[slot].last_us) slot = i;
    }
    strncpy(a->tokens[slot].tok, tok, AUTH_TOKEN_LEN);
    a->tokens[slot].tok[AUTH_TOKEN_LEN] = '\0';
    a->tokens[slot].last_us = now_us;
}

bool authc_token_ok(authc_t *a, const char *tok, int64_t now_us)
{
    if (!authc_enabled(a) || !tok || strlen(tok) != AUTH_TOKEN_LEN) return false;
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (!a->tokens[i].tok[0]) continue;
        if (now_us - a->tokens[i].last_us > AUTH_TOKEN_IDLE_US) {
            memset(&a->tokens[i], 0, sizeof(a->tokens[i]));
            continue;
        }
        if (same_bytes(a->tokens[i].tok, tok, AUTH_TOKEN_LEN)) {
            a->tokens[i].last_us = now_us;
            return true;
        }
    }
    return false;
}

void authc_revoke(authc_t *a, const char *tok)
{
    if (!tok) return;
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (a->tokens[i].tok[0] && strcmp(a->tokens[i].tok, tok) == 0) {
            memset(&a->tokens[i], 0, sizeof(a->tokens[i]));
        }
    }
}

authc_nut_t authc_nut_write(authc_t *a, const char *user, const char *pass,
                            int64_t now_us, authc_hash_fn hash)
{
    /* No login yet: NUT commands are refused, as upsd refuses them to a user
     * with no instcmds granted. Reads never come here. */
    if (!authc_enabled(a)) return AUTHC_NUT_DENIED;
    if (!user) return AUTHC_NUT_USERNAME_REQUIRED;
    if (!pass) return AUTHC_NUT_PASSWORD_REQUIRED;
    return authc_check(a, user, pass, now_us, hash) == AUTHC_OK
         ? AUTHC_NUT_ALLOW : AUTHC_NUT_DENIED;
}

bool authc_origin_ok(const char *origin, const char *host)
{
    if (!origin) return true;                    /* not a browser, or same-origin GET */
    if (!host || !host[0]) return false;
    const char *p = NULL;
    if (strncmp(origin, "http://", 7) == 0) p = origin + 7;
    else if (strncmp(origin, "https://", 8) == 0) p = origin + 8;
    if (!p) return false;                        /* includes Origin: null */
    return strcmp(p, host) == 0;
}
