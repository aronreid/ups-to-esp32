/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Long history's glue: the sampling task, the ring on flash, and the JSON a
 * chart reads. Format in history_core.h.
 *
 * WRITES WAIT FOR THE UPS'S USB TO GO QUIET, for the reason outagelog.c
 * gives: every erase and write is in flash_gated(), which only
 * flush_pending() calls, between ups_hid_quiet(true) and (false), and
 * tools/test/test_ota_usb_quiet.py fails if that changes. Readings are
 * batched -- thirty, five minutes' worth, per quiet spell -- so USB is
 * paused for a few milliseconds every five minutes. A power cut loses at
 * most the batch in RAM.
 *
 * READS ARE MEMORY-MAPPED, never esp_partition_read().
 */
#include "history.h"
#include "history_core.h"
#include "outagelog.h"
#include "ups_hid.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "history";

#define BATCH       30              /* readings per flush: five minutes */
#define PEND_CAP    (BATCH * 4)     /* room while a flush keeps failing */

static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_maph;
static uint32_t s_base;             /* the region's offset in the partition */
static uint32_t s_nsec;

static SemaphoreHandle_t s_lock;    /* everything below */
static uint32_t   s_cur;            /* sector being filled */
static hist_hdr_t s_hdr;            /* its header */
static bool       s_hdr_written;    /* on flash yet */
static uint32_t   s_count;          /* readings of it on flash */
/* In PSRAM. It is the source of esp_partition_write() in flash_gated(),
 * which is fine: esp_flash_write() sees a buffer outside internal RAM and
 * copies it in 32-byte pieces to the calling task's stack while the cache is
 * still on (esp_flash_api.c, direct_write false on SPI1). That task is
 * "history", whose stack is internal (xTaskCreate below). */
static EXT_RAM_BSS_ATTR hist_smp_t s_pend[PEND_CAP];
static uint32_t   s_npend;
static bool       s_new_sector;     /* start one with the next reading */
static uint32_t   s_write_errors;
static bool       s_ready;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static const uint8_t *sector_at(uint32_t i)
{
    return s_map + (size_t)i * HIST_SECTOR;
}

/* EVERY flash erase and write of the history is in here, and this runs only
 * from flush_pending(), between the quiet calls. Caller holds s_lock. */
static void flash_gated(void)
{
    size_t off = s_base + (size_t)s_cur * HIST_SECTOR;
    if (!s_hdr_written) {
        if (esp_partition_erase_range(s_part, off, HIST_SECTOR) != ESP_OK ||
            esp_partition_write(s_part, off, &s_hdr, sizeof(s_hdr)) != ESP_OK) {
            s_write_errors++;
            return;
        }
        s_hdr_written = true;
    }
    uint32_t n = s_npend;
    if (s_count + n > HIST_PER_SECTOR) n = HIST_PER_SECTOR - s_count;
    if (n && esp_partition_write(s_part, off + sizeof(hist_hdr_t) + s_count * sizeof(hist_smp_t),
                                 s_pend, n * sizeof(hist_smp_t)) != ESP_OK) {
        s_write_errors++;
        return;
    }
    s_count += n;
    s_npend -= n;
    memmove(s_pend, s_pend + n, s_npend * sizeof(hist_smp_t));
}

static void flush_pending(void)
{
    esp_err_t q = ups_hid_quiet(true);
    if (q == ESP_OK || q == ESP_ERR_INVALID_STATE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        flash_gated();
        xSemaphoreGive(s_lock);
    }
    ups_hid_quiet(false);                   /* always: quiet spells nest */
}

/* A new sector for the reading about to be taken. Caller holds s_lock, and
 * has flushed the old one: what is left in RAM belongs to it, and goes. */
static void begin_sector(void)
{
    uint32_t seq = s_hdr.seq + 1;
    s_cur = (s_cur + 1) % s_nsec;
    memset(&s_hdr, 0xFF, sizeof(s_hdr));
    s_hdr.seq = seq;
    s_hdr.epoch = outagelog_epoch_now();
    s_hdr.uptime_s = uptime_s();
    s_hdr.boot = outagelog_boot();
    s_hdr.period_s = HIST_PERIOD_S;
    memset(s_hdr.reserved, 0, sizeof(s_hdr.reserved));
    hist_seal(&s_hdr);
    s_hdr_written = false;
    s_count = 0;
    if (s_npend) s_write_errors++;
    s_npend = 0;
    s_new_sector = false;
}

static void sample(hist_in_t *in)
{
    static ups_data_t d;                    /* static: the task's stack is small */
    ups_hid_get(&d);
    int64_t now = esp_timer_get_time();
    memset(in, 0, sizeof(*in));
    in->present = d.attached && d.last_update_us && now - d.last_update_us < 60LL * 1000000;
    in->ol = d.status & UPS_STATUS_ONLINE;
    in->ob = d.status & UPS_STATUS_ONBATT;
    in->lb = d.status & UPS_STATUS_LOWBATT;
    in->chrg = d.status & UPS_STATUS_CHARGING;
    in->charge = d.battery_charge;
    in->load = d.ups_load;
    in->vin = d.input_voltage;
    in->runtime = d.battery_runtime;
    in->watts = ups_power_watts(&d, NULL);
}

static void history_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    bool had_clock = outagelog_epoch_now() != 0;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(HIST_PERIOD_S * 1000));
        hist_in_t in;
        sample(&in);
        hist_smp_t smp = hist_encode(&in);

        /* The clock just set: start a sector that carries it, so these
         * readings keep their time after a restart. */
        bool clock = outagelog_epoch_now() != 0;
        bool flush = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (clock && !had_clock && s_count + s_npend > 0) s_new_sector = true;
        bool full = s_count + s_npend >= HIST_PER_SECTOR;
        if ((s_new_sector || full) && s_npend) flush = true;
        xSemaphoreGive(s_lock);
        had_clock = clock;
        if (flush) flush_pending();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_new_sector || s_count + s_npend >= HIST_PER_SECTOR) begin_sector();
        if (s_npend < PEND_CAP) s_pend[s_npend++] = smp;
        else s_write_errors++;
        flush = s_npend >= BATCH;
        xSemaphoreGive(s_lock);
        if (flush) flush_pending();
    }
}

esp_err_t history_start(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, OUTAGELOG_PART_SUBTYPE,
                                      OUTAGELOG_PART_LABEL);
    if (!s_part || s_part->size <= OUTAGELOG_LOG_BYTES + HIST_SECTOR) {
        ESP_LOGE(TAG, "no room for history in the outagelog partition");
        return ESP_ERR_NOT_FOUND;
    }
    s_base = OUTAGELOG_LOG_BYTES;
    s_nsec = (s_part->size - s_base) / HIST_SECTOR;
    const void *p;
    esp_err_t err = esp_partition_mmap(s_part, s_base, (size_t)s_nsec * HIST_SECTOR,
                                       ESP_PARTITION_MMAP_DATA, &p, &s_maph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not map the history: %s", esp_err_to_name(err));
        return err;
    }
    s_map = p;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    uint32_t seq = 0;
    int32_t newest = hist_newest(s_map, s_nsec, &seq);
    /* A restart always starts a sector: the old one's readings came at its
     * own period from its own start, and this boot's cannot follow them. */
    s_cur = newest < 0 ? s_nsec - 1 : (uint32_t)newest;
    s_hdr.seq = newest < 0 ? 0 : seq;
    s_new_sector = true;                    /* begun by the first reading, at its time */
    ESP_LOGI(TAG, "%u sectors (%u days), newest %d (seq %u)", (unsigned)s_nsec,
             (unsigned)(s_nsec * HIST_PER_SECTOR * HIST_PERIOD_S / 86400), (int)newest, (unsigned)seq);

    s_ready = true;
    if (xTaskCreate(history_task, "history", 3072, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void history_status(history_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_ready) return;
    out->ready = true;
    out->sectors = s_nsec;
    out->days = s_nsec * HIST_PER_SECTOR * HIST_PERIOD_S / 86400;
    out->write_errors = s_write_errors;
}

/* ---- JSON ---- */

/* When a sector's first reading was, on the chart's clock: UTC if the board
 * has one, else this boot's uptime (only this boot's sectors then). 0 is
 * "cannot be placed". */
static uint32_t sector_time(const hist_hdr_t *h, bool clock, uint32_t now_t)
{
    uint16_t boot = outagelog_boot();
    if (clock) {
        if (h->epoch) return h->epoch;
        if (h->boot != boot) return 0;
        uint32_t up = uptime_s();
        return h->uptime_s <= up ? now_t - (up - h->uptime_s) : 0;
    }
    return h->boot == boot ? h->uptime_s + 1 : 0;   /* +1: uptime 0 is a real time */
}

static void add_sector(hist_agg_t *a, const hist_hdr_t *h, const hist_smp_t *s, uint32_t n,
                       bool clock, uint32_t now_t)
{
    uint32_t t = sector_time(h, clock, now_t);
    if (!t) return;
    for (uint32_t i = 0; i < n; i++) hist_agg_add(a, t + i * h->period_s, &s[i]);
}

void history_json(uint32_t span, uint32_t points, void (*emit)(void *, const char *), void *ctx)
{
    if (span < 600) span = 600;
    if (span > 31u * 86400) span = 31u * 86400;
    if (points < 10) points = 10;
    if (points > 600) points = 600;
    uint32_t step = span / points;
    if (step < HIST_PERIOD_S) step = HIST_PERIOD_S;
    points = (span + step - 1) / step;

    uint32_t epoch = outagelog_epoch_now();
    bool clock = epoch != 0;
    uint32_t now_t = clock ? epoch : uptime_s() + 1;
    char b[160];

    hist_bucket_t *store = s_ready ? heap_caps_malloc(points * sizeof(*store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    hist_smp_t *pend = s_ready ? heap_caps_malloc(PEND_CAP * sizeof(*pend), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!store || !pend) {
        free(store); free(pend);
        emit(ctx, "{\"ok\":false,\"error\":\"no history on this board\"}");
        return;
    }
    hist_agg_t a;
    uint32_t t0 = now_t > span ? now_t - span : 1;
    hist_agg_init(&a, store, points, t0, step);

    /* The sector being filled: what is on flash, then what is still in RAM. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t cur = s_cur, count = s_count, npend = s_npend;
    hist_hdr_t hdr = s_hdr;
    memcpy(pend, s_pend, npend * sizeof(*pend));
    xSemaphoreGive(s_lock);

    if (count && hist_hdr_valid(&hdr)) {
        add_sector(&a, &hdr, (const hist_smp_t *)(sector_at(cur) + sizeof(hist_hdr_t)), count, clock, now_t);
    }
    if (npend && hist_hdr_valid(&hdr)) {
        uint32_t t = sector_time(&hdr, clock, now_t);
        if (t) for (uint32_t i = 0; i < npend; i++) hist_agg_add(&a, t + (count + i) * hdr.period_s, &pend[i]);
    }
    /* Then older sectors, newest first, until they are older than the window. */
    for (uint32_t k = 1; k < s_nsec; k++) {
        uint32_t i = (cur + s_nsec - k) % s_nsec;
        const hist_hdr_t *h = (const hist_hdr_t *)sector_at(i);
        if (!hist_hdr_valid(h) || h->seq > hdr.seq || hdr.seq - h->seq >= s_nsec) continue;
        if (i == cur) continue;
        uint32_t n = hist_count(sector_at(i));
        uint32_t t = sector_time(h, clock, now_t);
        if (t && n && t + n * h->period_s < t0) break;
        add_sector(&a, h, (const hist_smp_t *)(sector_at(i) + sizeof(hist_hdr_t)), n, clock, now_t);
    }
    free(pend);

    snprintf(b, sizeof(b), "{\"ok\":true,\"clock\":%s,\"span\":%u,\"step\":%u,"
             "\"cols\":[\"ago\",\"charge\",\"load\",\"vin_min\",\"vin_max\",\"watts\",\"flags\"],\"rows\":[",
             clock ? "true" : "false", (unsigned)span, (unsigned)step);
    emit(ctx, b);
    bool first = true;
    for (uint32_t i = 0; i < a.nb; i++) {
        const hist_bucket_t *k = &a.b[i];
        if (!k->n) continue;
        uint32_t start = a.t0 + i * a.step;
        int len = snprintf(b, sizeof(b), "%s[%d", first ? "" : ",", -(int)(now_t - start));
        len += k->charge_n ? snprintf(b + len, sizeof(b) - len, ",%.1f", k->charge_sum / (double)k->charge_n)
                           : snprintf(b + len, sizeof(b) - len, ",null");
        len += k->load_n ? snprintf(b + len, sizeof(b) - len, ",%.1f", k->load_sum / (double)k->load_n)
                         : snprintf(b + len, sizeof(b) - len, ",null");
        len += k->vin_min != HIST_NONE16
                   ? snprintf(b + len, sizeof(b) - len, ",%.1f,%.1f", k->vin_min / 10.0, k->vin_max / 10.0)
                   : snprintf(b + len, sizeof(b) - len, ",null,null");
        len += k->watts_n ? snprintf(b + len, sizeof(b) - len, ",%.0f", k->watts_sum * 10.0 / k->watts_n)
                          : snprintf(b + len, sizeof(b) - len, ",null");
        snprintf(b + len, sizeof(b) - len, ",%u]", k->flags_or);
        emit(ctx, b);
        first = false;
    }
    free(store);
    emit(ctx, "]}");
}
