/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pure parts of the diagnostic log: a byte ring that keeps the newest
 * text, the gate that says whether anything may be captured or sent, the
 * upload rate limit, and the scrub that removes secrets. Free of ESP-IDF so
 * tools/test/test_diaglog.py compiles and runs them on the host.
 *
 * The ring does no locking and no allocation: diaglog.c holds a spinlock
 * around dl_ring_put() and gives it a buffer it allocated once.
 */
#ifndef UPSA_DIAGLOG_CORE_H
#define UPSA_DIAGLOG_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- the gate ------------------------------------------------------------
 * Capture and upload are allowed only when the owner chose "offer release
 * candidates", or the fleet server names this board a project test board.
 * A full-release board with neither runs none of this. */
/* Owner, 2026-11-26: the "offer release candidates" checkbox alone. The
 * fleet's dev tier does not open it (a family board sits on that tier); the
 * project's bench boards tick the checkbox like anyone else. */
static inline bool dl_gate(bool rc_opt)
{
    return rc_opt;
}

/* ---- the ring ---------------------------------------------------------- */
typedef struct {
    char    *buf;
    size_t   cap;
    uint32_t total;     /* bytes ever written; wraps after 4 GB, harmlessly */
} dl_ring_t;

static inline void dl_ring_init(dl_ring_t *r, char *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->total = 0;
}

static inline void dl_ring_put(dl_ring_t *r, const char *s, size_t n)
{
    if (!r->buf || !r->cap) return;
    if (n > r->cap) { s += n - r->cap; n = r->cap; }   /* only the newest fit */
    size_t pos = r->total % r->cap;
    size_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(r->buf + pos, s, first);
    if (n > first) memcpy(r->buf, s + first, n - first);
    r->total += (uint32_t)n;
}

/* Copy the newest text into out (NUL-terminated), oldest first, at most
 * out_cap - 1 bytes. When older text has been lost the copy starts at the
 * next whole line, so it never opens mid-line. Returns the length. */
static inline size_t dl_ring_read(const dl_ring_t *r, char *out, size_t out_cap)
{
    if (!out_cap) return 0;
    out[0] = '\0';
    if (!r->buf || !r->cap) return 0;
    size_t have = r->total < r->cap ? r->total : r->cap;
    size_t want = have < out_cap - 1 ? have : out_cap - 1;
    size_t start = (r->total - want) % r->cap;      /* ring index of the first byte */
    size_t first = r->cap - start < want ? r->cap - start : want;
    memcpy(out, r->buf + start, first);
    if (want > first) memcpy(out + first, r->buf, want - first);
    out[want] = '\0';
    bool mid = want < r->total && want > 0 &&
               r->buf[(start + r->cap - 1) % r->cap] != '\n';   /* cut inside a line */
    if (mid) {
        char *nl = memchr(out, '\n', want);
        if (nl) {
            size_t skip = (size_t)(nl - out) + 1;
            memmove(out, out + skip, want - skip + 1);
            want -= skip;
        }
    }
    return want;
}

/* ---- what goes into the ring -------------------------------------------------
 * ups_hid logs a status line ("ups_hid: batt ... | OL") every poll, about
 * every 2 s. Stored as is, it filled Rev A's 6 KB ring in ~2.5 minutes and
 * pushed out everything that mattered (bench, 2026-11-26). Keep one a minute,
 * plus any whose status (the text after the last '|') changed. The report
 * descriptor hex ups_hid prints at every attach (~45 lines, ~2.3 KB) is
 * dropped between its two marker lines, which are kept: the descriptor
 * itself reaches the server by its own upload. Every other line is kept. */
#define DL_STATUS_EVERY_S   60
#define DL_SHAPES          32
#define DL_REPEAT_EVERY_S  300
typedef struct {
    int64_t  last_s;        /* when a status line was last kept, -1 never */
    uint32_t last_hash;     /* hash of that line's status part */
    bool     in_desc;       /* between the descriptor dump's markers */
    int64_t  desc_s;        /* when that dump began */
    uint32_t shape[DL_SHAPES];      /* recent line shapes (numbers ignored), 0 empty */
    int64_t  shape_s[DL_SHAPES];    /* when each was last kept */
} dl_filter_t;

static inline void dl_filter_init(dl_filter_t *f)
{
    memset(f, 0, sizeof(*f));
    f->last_s = -1;
}

/* A line's shape: its text after the "I (123) " prefix with every run of
 * digits and hex letters next to digits ignored, so the same message with a
 * different number, address or index has one shape. */
static inline uint32_t dl__shape(const char *s, size_t n)
{
    size_t i = 0;
    if (n > 3 && s[1] == ' ' && s[2] == '(') {
        while (i < n && s[i] != ')') i++;
        i++;
    }
    uint32_t h = 2166136261u;
    bool in_num = false;
    for (; i < n; i++) {
        char c = s[i];
        bool dig = (c >= '0' && c <= '9') ||
                   (in_num && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        if (dig) { if (!in_num) h = (h ^ '#') * 16777619u; in_num = true; continue; }
        in_num = false;
        if (c == '\n' || c == '\r') continue;
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    return h ? h : 1;
}

/* True if this shape was kept within DL_REPEAT_EVERY_S; else records it. */
static inline bool dl__repeat(dl_filter_t *f, uint32_t sh, int64_t now_s)
{
    int oldest = 0;
    for (int i = 0; i < DL_SHAPES; i++) {
        if (f->shape[i] == sh) {
            if (now_s - f->shape_s[i] < DL_REPEAT_EVERY_S) return true;
            f->shape_s[i] = now_s;
            return false;
        }
        if (f->shape[i] == 0 || f->shape_s[i] < f->shape_s[oldest]) oldest = i;
    }
    f->shape[oldest] = sh;
    f->shape_s[oldest] = now_s;
    return false;
}

static inline bool dl__has(const char *s, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    for (size_t i = 0; i + l <= n; i++) if (memcmp(s + i, lit, l) == 0) return true;
    return false;
}

static inline bool dl_keep_line(dl_filter_t *f, const char *s, size_t n, int64_t now_s)
{
    /* Tailscale's routine chatter filled 170 of 200 uploaded lines on Rev B
     * (bench, 2026-11-26): its INFO lines (relay map, map requests, STUN
     * probes, peers) are left out; its warnings and errors stay. */
    if (n > 3 && s[0] == 'I' && (dl__has(s, n, ") ml_") || dl__has(s, n, ") wg: "))) {
        return false;
    }
    if (dl__has(s, n, "---- END REPORT DESCRIPTOR")) { f->in_desc = false; return true; }
    if (dl__has(s, n, "---- BEGIN REPORT DESCRIPTOR")) { f->in_desc = true; f->desc_s = now_s; return true; }
    if (f->in_desc && now_s - f->desc_s > 10) f->in_desc = false;   /* its END was lost */
    if (f->in_desc && dl__has(s, n, "ups_hid: ")) {
        /* The hex rows. Other tasks' lines print in between (Wi-Fi does at
         * boot), so they neither end the dump nor get dropped. */
        return false;
    }
    const char *tag = "ups_hid: batt ";
    size_t tl = strlen(tag);
    bool status = false;
    for (size_t i = 0; i + tl <= n && i < 40; i++) {
        if (memcmp(s + i, tag, tl) == 0) { status = true; break; }
    }
    if (!status) {
        /* Everything else: the same message at most once per
         * DL_REPEAT_EVERY_S (a periodic warning, a STALL every 30 s), except
         * the descriptor markers, which bracket a dump and must pair. */
        if (dl__has(s, n, "REPORT DESCRIPTOR")) return true;
        return !dl__repeat(f, dl__shape(s, n), now_s);
    }
    size_t bar = n;
    for (size_t i = n; i > 0; i--) if (s[i - 1] == '|') { bar = i; break; }
    uint32_t h = 2166136261u;
    for (size_t i = bar; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r') continue;
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    }
    if (f->last_s >= 0 && h == f->last_hash && now_s - f->last_s < DL_STATUS_EVERY_S) return false;
    f->last_s = now_s;
    f->last_hash = h;
    return true;
}

/* Internet addresses: the board's own public address (Tailscale's STUN
 * probe learns it), its peers' (WireGuard handshakes name them), relays'.
 * The privacy page promises the server never keeps a board's IP, so every
 * public IPv4 address and global IPv6 address is masked in place, keeping
 * the length. Private, tailnet (100.64/10), loopback and link-local
 * addresses stay: they say how the board's own network is laid out, which
 * is what a bug needs, and identify no one on the internet. */
static inline bool dl__isdig(char c) { return c >= '0' && c <= '9'; }
static inline bool dl__ishexc(char c)
{
    return dl__isdig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool dl__v4_public(const int o[4])
{
    if (o[0] == 10 || o[0] == 127 || o[0] == 0 || o[0] >= 224) return false;
    if (o[0] == 192 && o[1] == 168) return false;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return false;
    if (o[0] == 169 && o[1] == 254) return false;
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return false;     /* CGNAT, tailnet */
    if (o[0] == 255) return false;
    return true;
}

static inline void dl_redact_line(char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        /* IPv4: four dotted octets, not part of a longer number or name */
        if (dl__isdig(s[i]) && (i == 0 || !(dl__isdig(s[i - 1]) || s[i - 1] == '.'))) {
            int o[4] = {0}, k = 0;
            size_t j = i;
            bool ok = true;
            for (; k < 4; k++) {
                size_t st = j;
                int v = 0;
                while (j < n && dl__isdig(s[j]) && j - st < 3) v = v * 10 + (s[j++] - '0');
                if (j == st || v > 255) { ok = false; break; }
                o[k] = v;
                if (k < 3) { if (j < n && s[j] == '.') j++; else { ok = false; break; } }
            }
            if (ok && k == 4 && !(j < n && (dl__isdig(s[j]) || (s[j] == '.' && j + 1 < n && dl__isdig(s[j + 1]))))) {
                if (dl__v4_public(o)) memset(s + i, '*', j - i);
                i = j;
                continue;
            }
        }
        /* IPv6 global unicast (2000::/3): starts 2 or 3, hex and colons,
         * with "::" or at least four colons (a clock time has two) */
        if ((s[i] == '2' || s[i] == '3') && (i == 0 || !(dl__ishexc(s[i - 1]) || s[i - 1] == ':'))) {
            size_t j = i;
            int colons = 0;
            bool dbl = false;
            while (j < n && (dl__ishexc(s[j]) || s[j] == ':')) {
                if (s[j] == ':') { colons++; if (j > i && s[j - 1] == ':') dbl = true; }
                j++;
            }
            if (colons >= 2 && (dbl || colons >= 4)) {
                memset(s + i, '*', j - i);
                i = j;
                continue;
            }
        }
        i++;
    }
}

/* ---- the upload rate limit ------------------------------------------------
 * At most DL_MAX_PER_DAY uploads in any 24 h window of uptime, and a USB
 * failure at most once per DL_USB_DEBOUNCE_S. Times are seconds since boot. */
#define DL_MAX_PER_DAY      12
#define DL_DAY_S            86400
#define DL_USB_DEBOUNCE_S   600

typedef struct {
    int32_t  stamp[DL_MAX_PER_DAY];  /* when each of the last uploads went */
    int      n;                      /* how many are in stamp */
    int64_t  last_usb_s;             /* last accepted usb_fail request, -1 none */
} dl_limit_t;

static inline void dl_limit_init(dl_limit_t *l)
{
    memset(l, 0, sizeof(*l));
    l->last_usb_s = -1;
}

/* A request to upload for `reason`: refused if usb_fail came within ten
 * minutes of the last one. Does not count against the day; sending does. */
static inline bool dl_request_ok(dl_limit_t *l, const char *reason, int64_t now_s)
{
    if (strcmp(reason, "usb_fail") != 0) return true;
    if (l->last_usb_s >= 0 && now_s - l->last_usb_s < DL_USB_DEBOUNCE_S) return false;
    l->last_usb_s = now_s;
    return true;
}

/* May an upload go now? Drops stamps older than a day. */
static inline bool dl_send_ok(dl_limit_t *l, int64_t now_s)
{
    int k = 0;
    for (int i = 0; i < l->n; i++) {
        if (now_s - l->stamp[i] < DL_DAY_S) l->stamp[k++] = l->stamp[i];
    }
    l->n = k;
    return l->n < DL_MAX_PER_DAY;
}

static inline void dl_sent(dl_limit_t *l, int64_t now_s)
{
    if (l->n < DL_MAX_PER_DAY) l->stamp[l->n++] = (int32_t)now_s;
}

/* ---- the scrub ------------------------------------------------------------
 * In place, same length: secrets are overwritten with '*'. What it covers,
 * with the log line that can print each:
 *   name=value or "name":"value" where the name is, or ends in, password,
 *     passwd, passphrase, secret, token, community, key (node_key, nodekey,
 *     authkey, privkey, apikey), or is exactly pass, pwd, psk, user,
 *     username, login, authorization (PPP "user=", Tailscale "node_key=",
 *     MQTT, SNMP and page credentials should one ever be logged)
 *   the credential after Bearer or Basic
 *   tskey-... Tailscale auth keys
 *   user:pass@ inside a URL
 *   any run of 32 or more hex digits (WireGuard, node, machine and disco
 *     keys printed as hex) and any 40 or more characters of base64
 * Wi-Fi names, addresses and UPS identifiers are kept on purpose. */
static inline bool dl__isword(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline char dl__lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static inline bool dl__ends(const char *w, size_t n, const char *suf)
{
    size_t m = strlen(suf);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) if (dl__lc(w[n - m + i]) != suf[i]) return false;
    return true;
}
static inline bool dl__eq(const char *w, size_t n, const char *lit)
{
    return n == strlen(lit) && dl__ends(w, n, lit);
}
static inline bool dl__secret_name(const char *w, size_t n)
{
    static const char *const whole[] = { "pass", "pwd", "psk", "user", "username", "login",
                                         "authorization", NULL };
    static const char *const suffix[] = { "password", "passwd", "passphrase", "secret", "token",
                                          "community", "key", NULL };
    for (int i = 0; whole[i]; i++) if (dl__eq(w, n, whole[i])) return true;
    for (int i = 0; suffix[i]; i++) if (dl__ends(w, n, suffix[i])) return true;
    return false;
}
static inline bool dl__ishex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline bool dl__isb64(char c) { return dl__isword(c) || c == '+' || c == '-'; }
static inline void dl__mask(char *p, size_t n) { memset(p, '*', n); }
static inline size_t dl__valuelen(const char *p)
{
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' && p[n] != '\r' &&
           p[n] != '"' && p[n] != '\'' && p[n] != ',' && p[n] != ';' && p[n] != '&' &&
           p[n] != '}' && p[n] != ')') n++;
    return n;
}

static inline void diaglog_scrub(char *s)
{
    size_t len = strlen(s);
    /* Pass 1: names followed by = or :, Bearer/Basic, tskey-. */
    for (size_t i = 0; i < len; ) {
        if (!dl__isword(s[i])) { i++; continue; }
        size_t w = i;
        while (i < len && dl__isword(s[i])) i++;      /* the word */
        size_t wn = i - w;
        if (dl__eq(s + w, wn, "tskey") && i < len && s[i] == '-') {
            size_t e = i + 1;
            while (e < len && dl__isb64(s[e])) e++;
            dl__mask(s + w, e - w);
            i = e;
            continue;
        }
        if (dl__eq(s + w, wn, "bearer") || dl__eq(s + w, wn, "basic")) {
            size_t v = i;
            while (v < len && s[v] == ' ') v++;
            size_t vn = dl__valuelen(s + v);
            if (v > i) dl__mask(s + v, vn);
            i = v + vn;
            continue;
        }
        if (!dl__secret_name(s + w, wn)) continue;
        size_t j = i;
        if (j < len && s[j] == '"') j++;              /* "key": */
        while (j < len && s[j] == ' ') j++;
        if (j >= len || (s[j] != '=' && s[j] != ':')) continue;
        if (s[j] == ':' && s[j + 1] == '/') continue; /* a URL scheme, not a value */
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '"' || s[j] == '\'')) j++;
        size_t vn = dl__valuelen(s + j);
        if (dl__eq(s + w, wn, "authorization")) {      /* "Bearer xyz": all of it */
            vn = 0;
            while (s[j + vn] && s[j + vn] != '\n' && s[j + vn] != '\r') vn++;
        }
        dl__mask(s + j, vn);
        i = j + vn;
    }
    /* Pass 2: user:pass@host in a URL. */
    for (size_t i = 0; i + 3 < len; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') {
            size_t j = i + 3, e = j;
            while (e < len && s[e] != '/' && s[e] != ' ' && s[e] != '\n' && s[e] != '"' && s[e] != '@') e++;
            if (e < len && s[e] == '@') dl__mask(s + j, e - j);
        }
    }
    /* Pass 3: long hex and long base64 runs. */
    for (size_t i = 0; i < len; ) {
        if (!dl__isb64(s[i])) { i++; continue; }
        size_t b = i, hex = 0;
        while (i < len && dl__isb64(s[i])) { if (dl__ishex(s[i])) hex++; i++; }
        size_t n = i - b;
        while (i < len && s[i] == '=') i++;           /* base64 padding */
        if ((hex == n && n >= 32) || n >= 40) dl__mask(s + b, i - b);
    }
}

#endif /* UPSA_DIAGLOG_CORE_H */
