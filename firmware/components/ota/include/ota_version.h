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
#include <ctype.h>
#include <string.h>
#include <strings.h>

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

/* A tag as release.yml makes them: v, a digit, then letters, digits, dots and
 * dashes, under 32 characters. Anything else from a server is refused rather
 * than put in a URL, an error string or the page. */
static inline bool ota_tag_ok(const char *t)
{
    if (!t || t[0] != 'v' || !isdigit((unsigned char)t[1]) || strlen(t) > 31) return false;
    for (const char *p = t; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false;
    }
    return true;
}

/* Is the image the tag's own? release.yml stamps each image's version with
 * the tag it publishes under (-DPROJECT_VER), so an image whose own version is
 * not the tag is an old (or other) signed build re-uploaded under a newer tag:
 * a downgrade the signature check cannot see, since the signature is genuine.
 * `img_ver` is esp_app_desc_t.version, a 32-byte field that need not end in a
 * NUL. */
static inline bool ota_image_matches_tag(const char *img_ver, const char *tag)
{
    if (!img_ver || !tag || !img_ver[0] || !tag[0]) return false;
    return strncmp(img_ver, tag, 32) == 0;
}

/* May an image be downloaded from this URL? Only https, and only from
 * GitHub: github.com itself (a release asset's browser_download_url) or
 * objects.githubusercontent.com. The URL comes out of the release JSON, which
 * is only as trustworthy as the account behind it. (Redirects are not named
 * here: ESP-IDF's HTTP client refuses a redirect from an https URL to a
 * non-https one, which is the downgrade that mattered.) */
static inline bool ota_url_ok(const char *url)
{
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    const char *h = url + 8;
    size_t n = strcspn(h, "/:?#@");
    if (h[n] == '@') return false;                       /* user@host tricks */
    if (h[n] == ':' ) {                                   /* only the default port */
        if (strncmp(h + n, ":443", 4) != 0) return false;
        char after = h[n + 4];
        if (after != '/' && after != '\0') return false;
    }
    static const char *const ok[] = { "github.com", "objects.githubusercontent.com" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (n == strlen(ok[i]) && strncasecmp(h, ok[i], n) == 0) return true;
    }
    return false;
}

#endif
