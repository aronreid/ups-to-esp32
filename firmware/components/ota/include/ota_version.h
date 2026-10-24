/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure release-tag ordering, kept out of ota.c so it can be compiled and
 * tested on the host (tools/test/test_ota_version.py).
 *
 * Used for one decision only: may a board install a fleet-named release by
 * itself? Only if the tag is provably newer than what is running. A person
 * pressing Install is never checked, so rolling back by hand still works.
 *
 * Understands what release.yml publishes: v0.16, v0.15.1, v0.16-rc5. A
 * release candidate is older than the release it leads to. Everything else
 * (a hash, "-dirty", "-hub-dev", an empty string) does not parse, and an
 * unparseable tag or running version is never "newer".
 */
#ifndef UPSA_OTA_VERSION_H
#define UPSA_OTA_VERSION_H

#include <stdbool.h>

#define OTA_VER_PARTS 4

typedef struct {
    long n[OTA_VER_PARTS];  /* numeric parts, missing ones 0 */
    long rc;                /* 0 for a release, else the candidate number */
} ota_ver_t;

static inline bool ota_ver_parse(const char *s, ota_ver_t *v)
{
    if (!s || s[0] != 'v') return false;
    s++;
    for (int i = 0; i < OTA_VER_PARTS; i++) v->n[i] = 0;
    v->rc = 0;
    int parts = 0;
    for (;;) {
        if (*s < '0' || *s > '9') return false;
        long x = 0;
        for (int d = 0; *s >= '0' && *s <= '9'; s++) {
            if (++d > 6) return false;
            x = x * 10 + (*s - '0');
        }
        if (parts >= OTA_VER_PARTS) return false;
        v->n[parts++] = x;
        if (*s != '.') break;
        s++;
    }
    if (*s == '\0') return true;
    if (s[0] != '-' || s[1] != 'r' || s[2] != 'c') return false;
    s += 3;
    if (*s < '1' || *s > '9') return false;
    long r = 0;
    for (int d = 0; *s >= '0' && *s <= '9'; s++) {
        if (++d > 6) return false;
        r = r * 10 + (*s - '0');
    }
    if (*s != '\0') return false;
    v->rc = r;
    return true;
}

/* True only when `cand` parses, `running` parses, and cand is strictly newer. */
static inline bool ota_tag_newer(const char *cand, const char *running)
{
    ota_ver_t c, r;
    if (!ota_ver_parse(cand, &c) || !ota_ver_parse(running, &r)) return false;
    for (int i = 0; i < OTA_VER_PARTS; i++) {
        if (c.n[i] != r.n[i]) return c.n[i] > r.n[i];
    }
    if (c.rc == r.rc) return false;
    if (c.rc == 0) return true;     /* a release beats its candidates */
    if (r.rc == 0) return false;
    return c.rc > r.rc;
}

#endif
