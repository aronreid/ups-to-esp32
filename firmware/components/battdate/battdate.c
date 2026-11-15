/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "battdate.h"
#include "battdate_core.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "battdate";

/* In "upsa" with the board's other settings, so a factory reset clears it.
 * Key "bd" + 8 hex digits of bd_key_hash(): one record per UPS. Rev A's NVS is
 * small, so at most BD_KEEP records are kept; the least recently saved goes. */
#define NVS_NS   "upsa"
#define BD_KEEP  8
#define BD_VER   1

/* The stored record, packed by hand so its layout does not depend on the
 * compiler: ver, years, fitted (i32 LE), due (i32 LE), seq (u32 LE). */
#define REC_LEN  14
typedef struct {
    uint8_t  years;
    int32_t  fitted;   /* board-kept fitted date, BD_NONE if none */
    int32_t  due;      /* fixed due date, BD_NONE if none */
    uint32_t seq;      /* save order, for evicting the oldest */
} rec_t;

/* A few UPSes' records, for the hub build's several UPSes; one is the norm.
 * NUT workers, the web server and /metrics read it: a spinlock, held only to
 * copy a record in or out, never across NVS. Saves come only from the web
 * server, whose requests are already one at a time (webui.c s_req_lock). */
#define CACHE_N 4
static struct { bool used; uint32_t key; rec_t r; } s_cache[CACHE_N];
static int s_cache_next;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void rec_default(rec_t *r)
{
    r->years = BD_YEARS_DEF; r->fitted = BD_NONE; r->due = BD_NONE; r->seq = 0;
}

static void key_name(uint32_t key, char out[12]) { snprintf(out, 12, "bd%08lx", (unsigned long)key); }

static bool nvs_load(uint32_t key, rec_t *r)
{
    rec_default(r);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char k[12]; key_name(key, k);
    uint8_t b[REC_LEN]; size_t len = sizeof(b);
    bool ok = nvs_get_blob(h, k, b, &len) == ESP_OK && len == REC_LEN && b[0] == BD_VER;
    nvs_close(h);
    if (!ok) return false;
    r->years  = (b[1] >= BD_YEARS_MIN && b[1] <= BD_YEARS_MAX) ? b[1] : BD_YEARS_DEF;
    r->fitted = (int32_t)get32(b + 2);
    r->due    = (int32_t)get32(b + 6);
    r->seq    = get32(b + 10);
    return true;
}

static void cache_put(uint32_t key, const rec_t *r)
{
    taskENTER_CRITICAL(&s_mux);
    int i = -1;
    for (int j = 0; j < CACHE_N; j++) if (s_cache[j].used && s_cache[j].key == key) { i = j; break; }
    if (i < 0) { i = s_cache_next; s_cache_next = (s_cache_next + 1) % CACHE_N; }
    s_cache[i].used = true; s_cache[i].key = key; s_cache[i].r = *r;
    taskEXIT_CRITICAL(&s_mux);
}

/* The record for this UPS: from the cache, else read from NVS and cached. */
static void rec_for(uint32_t key, rec_t *out)
{
    bool hit = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CACHE_N; i++) {
        if (s_cache[i].used && s_cache[i].key == key) { *out = s_cache[i].r; hit = true; break; }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (hit) return;
    nvs_load(key, out);
    cache_put(key, out);
}

static uint32_t key_of(const ups_data_t *d) { return bd_key_hash(d->vid, d->pid, d->mfr, d->model, d->serial); }

/* The UPS's own fitted date, if it stores one and it is a real date. */
static int32_t ups_fitted(const ups_data_t *d)
{
    int32_t v;
    if ((d->caps & UPS_CAP_BATTDATE) && bd_parse(d->battery_mfr_date, &v)) return v;
    return BD_NONE;
}

void battdate_get(const ups_data_t *d, battdate_t *out)
{
    memset(out, 0, sizeof(*out));
    rec_t r;
    rec_for(key_of(d), &r);

    out->ups_has_date = (d->caps & UPS_CAP_BATTDATE) != 0;
    int32_t fitted = ups_fitted(d);
    if (fitted == BD_NONE && r.fitted != BD_NONE) { fitted = r.fitted; out->fitted_board = true; }
    out->years = r.years;
    out->due_fixed = r.due != BD_NONE;
    int32_t due = bd_due(fitted, r.years, r.due);
    bd_format(fitted, out->fitted, sizeof(out->fitted));
    bd_format(due, out->due, sizeof(out->due));

    int32_t today = bd_today((int64_t)time(NULL));
    out->clock = today != BD_NONE;
    if (out->clock && due != BD_NONE) {
        out->days_left = due - today;
        out->overdue = out->days_left <= 0;
    }
}

const char *battdate_alarm(const battdate_t *b)
{
    return b->overdue ? "Battery replacement due!" : NULL;
}

/* Drop the least recently saved record when BD_KEEP are stored and a new UPS
 * needs one. Returns the next save sequence number. */
static uint32_t make_room(nvs_handle_t h, const char *mine)
{
    uint32_t top = 0, low = UINT32_MAX; char low_key[16] = ""; int n = 0; bool have_mine = false;
    nvs_iterator_t it = NULL;
    for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_TYPE_BLOB, &it);
         e == ESP_OK; e = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "bd", 2) != 0 || strlen(info.key) != 10) continue;
        uint8_t b[REC_LEN]; size_t len = sizeof(b);
        if (nvs_get_blob(h, info.key, b, &len) != ESP_OK || len != REC_LEN) continue;
        uint32_t seq = get32(b + 10);
        n++;
        if (seq > top) top = seq;
        if (strcmp(info.key, mine) == 0) { have_mine = true; continue; }
        if (seq < low) { low = seq; strlcpy(low_key, info.key, sizeof(low_key)); }
    }
    nvs_release_iterator(it);
    if (!have_mine && n >= BD_KEEP && low_key[0]) {
        ESP_LOGI(TAG, "%d battery records kept: dropping the oldest, %s", n, low_key);
        nvs_erase_key(h, low_key);
    }
    return top + 1;
}

esp_err_t battdate_set(const ups_data_t *d, const char *fitted, int years,
                       const char *due, char *why, size_t why_cap)
{
    int32_t f = BD_NONE, u = BD_NONE;
    if (fitted && fitted[0] && !bd_parse(fitted, &f)) {
        strlcpy(why, "the fitted date must be a real date from 1980 to 2107", why_cap);
        return ESP_ERR_INVALID_ARG;
    }
    if (fitted && fitted[0] && (d->caps & UPS_CAP_BATTDATE)) {
        strlcpy(why, "this UPS stores its own battery date: set it on the UPS", why_cap);
        return ESP_ERR_INVALID_ARG;
    }
    if (due && due[0] && !bd_parse(due, &u)) {
        strlcpy(why, "the replace-by date must be a real date from 1980 to 2107", why_cap);
        return ESP_ERR_INVALID_ARG;
    }
    if (years >= 0 && (years < BD_YEARS_MIN || years > BD_YEARS_MAX)) {
        snprintf(why, why_cap, "replace every %d to %d years", BD_YEARS_MIN, BD_YEARS_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t key = key_of(d);
    rec_t r;
    rec_for(key, &r);
    if (fitted) r.fitted = fitted[0] ? f : BD_NONE;
    if (due)    r.due    = due[0] ? u : BD_NONE;
    if (years >= 0) r.years = (uint8_t)years;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char k[12]; key_name(key, k);
        r.seq = make_room(h, k);
        uint8_t b[REC_LEN] = { BD_VER, r.years };
        put32(b + 2, (uint32_t)r.fitted); put32(b + 6, (uint32_t)r.due); put32(b + 10, r.seq);
        err = nvs_set_blob(h, k, b, sizeof(b));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) cache_put(key, &r);
    if (err != ESP_OK) {
        snprintf(why, why_cap, "could not save (%s)", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
