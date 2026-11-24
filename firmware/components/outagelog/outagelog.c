/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The outage log's glue: the partition, the board's clock, the task that
 * watches the UPS, and the JSON the page reads. The record format, the ring
 * and the outage logic are outagelog_core.c's.
 *
 * WRITES WAIT FOR THE UPS'S USB TO GO QUIET. v0.14.4 panicked in ESP-IDF's
 * USB host driver every time it wrote flash for an update with a UPS's
 * interrupt pipe running (ups_hid_quiet()), and this log writes flash in the
 * middle of an outage -- the worst moment for the board to reboot, with every
 * NUT client watching. So every erase and write is in flash_gated(), which
 * only flush_pending() calls, between ups_hid_quiet(true) and (false), and
 * tools/test/test_ota_usb_quiet.py fails if that changes. A record that
 * cannot be written yet waits in RAM for the next try.
 *
 * READS ARE MEMORY-MAPPED. esp_partition_read() takes the flash off the
 * cache as a write does; reading through the MMU does not, so the boot scan
 * and the page's requests never stall the USB interrupt.
 */
#include "outagelog.h"
#include "outagelog_core.h"
#include "ups_hid.h"
#include "esp_partition.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "outagelog";

#define INDEX_CAP       4096        /* outages kept in the index, in PSRAM */
#define PEND_CAP        32          /* records waiting for a quiet moment */
#define NVS_NS          "olog"
#define CLOCK_VALID     1700000000u /* 2023: anything earlier is an unset clock */

static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_maph;
static uint32_t s_nslots;

static SemaphoreHandle_t s_lock;    /* everything below */
static uint32_t s_next_slot, s_next_seq, s_written;
static uint32_t s_min_seq;
static uint16_t s_boot;
static olog_index_t s_ix;
static olog_sm_t s_sm;
/* In PSRAM: flash_gated() copies each record to a stack local before
 * esp_partition_write(), so the cache-off write never reads this. */
static EXT_RAM_BSS_ATTR olog_rec_t s_pend[PEND_CAP];
static int s_npend;
static uint32_t s_write_errors;
static olog_wr_t s_wr;              /* the writer after a failure */
static bool s_ready;

uint32_t outagelog_epoch_now(void)
{
    time_t t = time(NULL);
    return t >= (time_t)CLOCK_VALID ? (uint32_t)t : 0;
}

uint16_t outagelog_boot(void)
{
    return s_boot;
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* A time for something seen `up` seconds into this boot: by the clock if it
 * is set now, whenever that was. */
static uint32_t epoch_at(uint32_t up)
{
    uint32_t now = outagelog_epoch_now(), up_now = uptime_s();
    if (!now || up > up_now) return 0;
    return now - (up_now - up);
}

static const olog_rec_t *slot_rec(uint32_t slot)
{
    return (const olog_rec_t *)(s_map + (size_t)slot * OLOG_REC_SIZE);
}

/* ---- writing ---- */

/* Records in the sector starting at `first` that the page counts. */
static uint32_t live_in_sector(uint32_t first)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < OLOG_PER_SECTOR; k++) {
        const olog_rec_t *r = slot_rec(first + k);
        if (olog_valid(r) && r->seq >= s_min_seq) n++;
    }
    return n;
}

/* A sector was erased: what was in it goes from the index and the count.
 * `gone` was counted before the erase. Caller holds s_lock. */
static void erased_sector(uint32_t first, uint32_t gone)
{
    olog_index_forget(&s_ix, first, s_nslots);
    s_written = s_written > gone ? s_written - gone : 0;
}

/* EVERY flash erase and write of this log is in here, and this runs only
 * from flush_pending(), between the quiet calls. Caller holds s_lock.
 *
 * What a failed write does next is olog_wr_done()'s, in outagelog_core.c
 * (host-tested), where the reasoning about power cuts and reboots is. */
static void flash_gated(bool preerase)
{
    int64_t now = esp_timer_get_time();
    while (s_npend > 0) {
        olog_rec_t r = s_pend[0];
        uint32_t slot = s_next_slot;
        size_t off = (size_t)slot * OLOG_REC_SIZE;
        bool erase = slot % OLOG_PER_SECTOR == 0 && !olog_erased(s_map + off, OLOG_SECTOR);
        if (!olog_wr_may(&s_wr, erase, now)) break;
        /* A power cut during the erase leaves the sector part-erased: the
         * next boot's scan finds the end before it again, and this slot gets
         * the same check, so the sector is erased again. */
        if (erase) {
            uint32_t gone = live_in_sector(slot);
            if (esp_partition_erase_range(s_part, off, OLOG_SECTOR) != ESP_OK) {
                s_write_errors++;
                olog_wr_done(&s_wr, OLOG_WR_UNTOUCHED, now);    /* the same slot, later */
                break;
            }
            erased_sector(slot, gone);
        }
        r.seq = s_next_seq;
        olog_seal(&r);
        esp_err_t err = esp_partition_write(s_part, off, &r, sizeof(r));
        const olog_rec_t *on = slot_rec(slot);
        olog_wr_res_t res = err == ESP_OK && olog_valid(on) ? OLOG_WR_GOOD
                          : olog_erased(on, OLOG_REC_SIZE) ? OLOG_WR_UNTOUCHED : OLOG_WR_TORN;
        s_next_slot = (slot + olog_wr_done(&s_wr, res, now)) % s_nslots;
        if (res != OLOG_WR_GOOD) {
            s_write_errors++;
            ESP_LOGE(TAG, "record %u did not write at slot %u (%s, %s)%s", (unsigned)r.seq,
                     (unsigned)slot, esp_err_to_name(err),
                     res == OLOG_WR_TORN ? "part-written" : "nothing written",
                     s_wr.halted ? ": the flash keeps failing, no more writes until a restart" : "");
            break;                          /* the record stays queued */
        }
        s_next_seq++;
        s_written++;
        olog_index_add(&s_ix, on, slot);
        memmove(&s_pend[0], &s_pend[1], (size_t)(--s_npend) * sizeof(s_pend[0]));
    }

    /* Between outages, erase the next sector ahead of need, so a long outage
     * writes into erased flash and rarely has to erase in the middle. Not
     * after a failed write (that would be an erase for a failed write), nor
     * during the wait after one. */
    if (preerase && s_npend == 0 && s_next_slot % OLOG_PER_SECTOR != 0 &&
        s_wr.ahead == 0 && olog_wr_may(&s_wr, false, now)) {
        uint32_t sec = (s_next_slot / OLOG_PER_SECTOR + 1) % (s_nslots / OLOG_PER_SECTOR);
        size_t off = (size_t)sec * OLOG_SECTOR;
        if (!olog_erased(s_map + off, OLOG_SECTOR)) {
            uint32_t gone = live_in_sector(sec * OLOG_PER_SECTOR);
            if (esp_partition_erase_range(s_part, off, OLOG_SECTOR) != ESP_OK) {
                s_write_errors++;
                olog_wr_done(&s_wr, OLOG_WR_UNTOUCHED, now);
            } else {
                erased_sector(sec * OLOG_PER_SECTOR, gone);
            }
        }
    }
}

static bool next_sector_dirty(void)
{
    if (s_next_slot % OLOG_PER_SECTOR == 0) return false;
    uint32_t sec = (s_next_slot / OLOG_PER_SECTOR + 1) % (s_nslots / OLOG_PER_SECTOR);
    return !olog_erased(s_map + (size_t)sec * OLOG_SECTOR, OLOG_SECTOR);
}

/* Write what is waiting, with the UPS's USB quiet. Called from the log task
 * only. ESP_ERR_INVALID_STATE from ups_hid_quiet() means USB never started
 * this boot, which is as quiet as it gets. USB is left alone while the
 * writer waits after a failure, or has stopped. */
static void flush_pending(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool may = !s_wr.halted && now >= s_wr.retry_us;
    bool preerase = may && s_wr.ahead == 0 && !s_sm.active && next_sector_dirty();
    bool work = (may && s_npend > 0) || preerase;
    xSemaphoreGive(s_lock);
    if (!work) return;

    esp_err_t q = ups_hid_quiet(true);
    if (q == ESP_OK || q == ESP_ERR_INVALID_STATE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        flash_gated(preerase);
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGW(TAG, "UPS USB would not go quiet (%s); %d record(s) wait",
                 esp_err_to_name(q), s_npend);
    }
    ups_hid_quiet(false);                   /* always: quiet spells nest */
}

/* Queue a record from the state machine. Caller holds s_lock. Full means the
 * flash has refused for a while: a START or END matters more than a sample,
 * so the newest sample waiting gives way. */
static void queue(olog_rec_t *r)
{
    if (!(r->flags & OLOG_F_COPIED)) {
        r->boot = s_boot;
        r->epoch = epoch_at(r->uptime_s);
    }
    if (s_npend == PEND_CAP) {
        int victim = -1;
        for (int i = s_npend - 1; i >= 0; i--) {
            if (s_pend[i].type == OLOG_SAMPLE) { victim = i; break; }
        }
        s_write_errors++;
        if (victim < 0 || r->type == OLOG_SAMPLE) return;
        memmove(&s_pend[victim], &s_pend[victim + 1], (size_t)(s_npend - victim - 1) * sizeof(s_pend[0]));
        s_npend--;
    }
    s_pend[s_npend++] = *r;
}

/* ---- watching ---- */

static void read_ups(olog_in_t *in)
{
    static ups_data_t d;                /* static: the task's stack is small */
    ups_hid_get(&d);
    int64_t now = esp_timer_get_time();
    memset(in, 0, sizeof(*in));
    /* No status bits, or readings a minute old, is "do not know", not mains:
     * an all-zero status after status_hold() gives up must not end an
     * outage as if the power came back. The state machine treats unknown
     * as the UPS being away. */
    in->attached = d.attached && d.status != UPS_STATUS_UNKNOWN &&
                   d.last_update_us && now - d.last_update_us < 60LL * 1000000;
    in->onbatt   = (d.status & UPS_STATUS_ONBATT) != 0;
    in->lowbatt  = (d.status & UPS_STATUS_LOWBATT) != 0;
    in->testing  = d.test_result == UPS_TEST_IN_PROGRESS;
    in->clock    = outagelog_epoch_now() != 0;
    in->charge   = d.battery_charge;
    in->load     = d.ups_load;
    in->runtime  = d.battery_runtime;
    in->vin      = d.input_voltage;
    in->vbat     = d.battery_voltage;
}

static void log_task(void *arg)
{
    bool was_active = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        olog_in_t in;
        read_ups(&in);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        olog_rec_t out[2];
        int n = olog_step(&s_sm, &in, esp_timer_get_time(), out);
        for (int i = 0; i < n; i++) queue(&out[i]);
        bool active = s_sm.active;
        xSemaphoreGive(s_lock);

        if (active != was_active) {
            ESP_LOGW(TAG, "outage %u: %s", (unsigned)s_sm.outage,
                     active ? "on battery" : "over");
            was_active = active;
        }
        flush_pending();
    }
}

/* ---- start ---- */

/* The boot number is only worth anything if the next boot cannot get the
 * same one: if NVS will not open, or will not keep the new number, this
 * boot is 0, "unknown", which nothing takes for the same boot as anything
 * (olog_index_add, put_time). Times across it then come from the clock or
 * not at all, never from subtracting two boots' uptimes. A real boot is
 * never 0: the count skips it when it wraps. */
static void nvs_load(void)
{
    nvs_handle_t h;
    uint16_t boot = 0;
    s_boot = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no NVS: this boot's number is unknown");
        return;
    }
    nvs_get_u16(h, "boot", &boot);
    boot++;
    if (boot == 0 || boot == OLOG_NONE16) boot = 1;
    nvs_get_u32(h, "minseq", &s_min_seq);
    if (nvs_set_u16(h, "boot", boot) == ESP_OK && nvs_commit(h) == ESP_OK) s_boot = boot;
    else ESP_LOGE(TAG, "boot number not kept: this boot's number is unknown");
    nvs_close(h);
}

esp_err_t outagelog_start(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, OUTAGELOG_PART_SUBTYPE,
                                      OUTAGELOG_PART_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "no outagelog partition: this board keeps no outage log");
        return ESP_ERR_NOT_FOUND;
    }
    /* First: the history stamps its sectors with this boot's number even if the
     * log cannot start below. */
    nvs_load();

    /* Only this log's share of the partition; the history has the rest. */
    uint32_t bytes = s_part->size < OUTAGELOG_LOG_BYTES ? s_part->size : OUTAGELOG_LOG_BYTES;
    const void *p;
    esp_err_t err = esp_partition_mmap(s_part, 0, bytes, ESP_PARTITION_MMAP_DATA, &p, &s_maph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not map the log: %s", esp_err_to_name(err));
        return err;
    }
    s_map = p;
    s_nslots = (bytes / OLOG_SECTOR) * OLOG_PER_SECTOR;

    size_t cap = INDEX_CAP;
    olog_outage_t *store = heap_caps_calloc(cap, sizeof(olog_outage_t), MALLOC_CAP_SPIRAM);
    if (!store) {                       /* no PSRAM: a short index in internal RAM */
        cap = INDEX_CAP / 16;
        store = heap_caps_calloc(cap, sizeof(olog_outage_t), MALLOC_CAP_DEFAULT);
    }
    if (!store) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    int64_t t0 = esp_timer_get_time();
    olog_scan_t sc;
    olog_scan(s_map, s_nslots, &sc);
    s_next_slot = sc.next_slot;
    s_next_seq = sc.next_seq;
    olog_index_init(&s_ix, store, cap, s_min_seq);
    const olog_rec_t *last = olog_index_build(&s_ix, s_map, s_nslots, &sc, &s_written);

    memset(&s_sm, 0, sizeof(s_sm));
    if (last) {
        s_sm.outage = last->outage;         /* new outages number on from here */
        if (last->type != OLOG_END && last->seq >= s_min_seq) {
            esp_reset_reason_t rr = esp_reset_reason();
            bool power_lost = rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT;
            olog_resume(&s_sm, last, power_lost);
            ESP_LOGW(TAG, "outage %u was open when this board went down (%s)",
                     (unsigned)last->outage, power_lost ? "power lost" : "restart");
        }
    }
    ESP_LOGI(TAG, "%u outages, %u records of %u, next slot %u; scanned in %d ms; boot %u",
             (unsigned)s_ix.n, (unsigned)s_written, (unsigned)s_nslots, (unsigned)s_next_slot,
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)s_boot);

    /* The board's clock, for the log's times. Retries by itself, so it does
     * not matter that Wi-Fi is not up yet. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) ESP_LOGW(TAG, "SNTP did not start: no clock for the log");

    s_ready = true;
    if (xTaskCreate(log_task, "outagelog", 4096, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void outagelog_status(outagelog_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->clock = outagelog_epoch_now() != 0;
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->ready = true;
    out->active = s_sm.active && !s_sm.resume;
    out->outages = s_ix.n;
    out->records = s_written;
    out->capacity = s_nslots;
    out->write_errors = s_write_errors;
    xSemaphoreGive(s_lock);
}

esp_err_t outagelog_clear(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t min = s_next_seq;
    xSemaphoreGive(s_lock);

    /* NVS writes flash too: the same quiet as the log's own writes. */
    esp_err_t q = ups_hid_quiet(true);
    esp_err_t err = ESP_FAIL;
    if (q == ESP_OK || q == ESP_ERR_INVALID_STATE) {
        nvs_handle_t h;
        err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_u32(h, "minseq", min);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
    }
    ups_hid_quiet(false);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_min_seq = min;
    olog_index_init(&s_ix, s_ix.v, s_ix.cap, min);
    s_written = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "cleared: records before %u are forgotten", (unsigned)min);
    return ESP_OK;
}

/* ---- JSON ---- */

static const char *reason_name(const olog_outage_t *o, bool open_now)
{
    switch (o->reason) {
    case OLOG_END_MAINS:      return "mains";
    case OLOG_END_UPS_LOST:   return "ups_lost";
    case OLOG_END_POWER_LOST: return "power_lost";
    case OLOG_END_RESTART:    return "restart";
    default:                  return open_now ? "ongoing" : "unknown";
    }
}

static void put_u8(char *b, size_t n, const char *k, uint8_t v)
{
    if (v == OLOG_NONE8) snprintf(b, n, ",\"%s\":null", k);
    else snprintf(b, n, ",\"%s\":%u", k, v);
}

static void put_x(char *b, size_t n, const char *k, uint16_t v, int div)
{
    if (v == OLOG_NONE16) snprintf(b, n, ",\"%s\":null", k);
    else if (div == 1) snprintf(b, n, ",\"%s\":%u", k, v);
    else snprintf(b, n, ",\"%s\":%.*f", k, div == 10 ? 1 : 2, v / (double)div);
}

static void put_time(char *b, size_t n, const char *k, uint32_t epoch, uint16_t boot, uint32_t up)
{
    if (!epoch && boot && boot == s_boot) epoch = epoch_at(up);
    if (epoch) snprintf(b, n, ",\"%s\":%u", k, (unsigned)epoch);
    else snprintf(b, n, ",\"%s\":null", k);
}

void outagelog_json_list(void (*emit)(void *, const char *), void *ctx, size_t max)
{
    char b[96];
    outagelog_status_t st;
    outagelog_status(&st);
    /* A copy, so a slow browser never holds up the task watching the UPS.
     * Without room for it, say so: an empty list with ready:true read as
     * "no outages recorded". */
    if (max > 200) max = 200;
    if (max < 1) max = 1;
    olog_outage_t *v = NULL;
    if (st.ready) {
        v = heap_caps_malloc(max * sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!v) {
            emit(ctx, "{\"ok\":false,\"error\":\"no memory for the outage list\"}");
            return;
        }
    }
    snprintf(b, sizeof(b), "{\"ok\":true,\"ready\":%s,\"clock\":%s,\"now\":%u,\"active\":%s,",
             st.ready ? "true" : "false", st.clock ? "true" : "false",
             (unsigned)outagelog_epoch_now(), st.active ? "true" : "false");
    emit(ctx, b);
    snprintf(b, sizeof(b), "\"records\":%u,\"capacity\":%u,\"write_errors\":%u,\"outages\":[",
             (unsigned)st.records, (unsigned)st.capacity, (unsigned)st.write_errors);
    emit(ctx, b);

    if (v) {
        size_t n = 0;
        uint32_t live_id = 0, live_extra = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (; n < max; n++) {
            const olog_outage_t *o = olog_index_get(&s_ix, n);
            if (!o) break;
            v[n] = *o;
        }
        if (s_sm.active && !s_sm.resume) {
            live_id = s_sm.outage;
            const olog_outage_t *o = olog_index_find(&s_ix, live_id);
            if (o && s_boot && o->end_boot == s_boot && uptime_s() > o->end_up) live_extra = uptime_s() - o->end_up;
        }
        xSemaphoreGive(s_lock);
        for (size_t i = 0; i < n; i++) {
            const olog_outage_t *o = &v[i];
            bool live = o->outage == live_id && o->reason == OLOG_END_OPEN;
            snprintf(b, sizeof(b), "%s{\"id\":%u", i ? "," : "", (unsigned)o->outage);
            emit(ctx, b);
            put_time(b, sizeof(b), "start", o->start_epoch, o->start_boot, o->start_up); emit(ctx, b);
            put_time(b, sizeof(b), "end", o->end_epoch, o->end_boot, o->end_up);         emit(ctx, b);
            snprintf(b, sizeof(b), ",\"dur\":%u,\"gap\":%s,\"open\":%s,\"reason\":\"%s\"",
                     (unsigned)(o->dur_s + (live ? live_extra : 0)), o->gap ? "true" : "false",
                     live ? "true" : "false", reason_name(o, live));
            emit(ctx, b);
            put_u8(b, sizeof(b), "c0", o->charge0);      emit(ctx, b);
            put_u8(b, sizeof(b), "c1", o->charge1);      emit(ctx, b);
            put_u8(b, sizeof(b), "cmin", o->charge_min); emit(ctx, b);
            put_u8(b, sizeof(b), "lmax", o->load_max);   emit(ctx, b);
            put_x(b, sizeof(b), "rt0", o->rt0, 1);       emit(ctx, b);
            put_x(b, sizeof(b), "rt1", o->rt1, 1);       emit(ctx, b);
            put_x(b, sizeof(b), "vin0", o->vin0, 10);    emit(ctx, b);
            snprintf(b, sizeof(b), ",\"lb\":%s,\"test\":%s,\"resumed\":%s,\"n\":%u}",
                     (o->flags & OLOG_F_LOWBATT) ? "true" : "false",
                     (o->flags & OLOG_F_TEST) ? "true" : "false",
                     (o->flags & OLOG_F_RESUMED) ? "true" : "false", (unsigned)o->records);
            emit(ctx, b);
        }
        free(v);
    }
    emit(ctx, "]}");
}

#define MAX_SAMPLES 4000

bool outagelog_json_samples(uint32_t outage, void (*emit)(void *, const char *), void *ctx)
{
    if (!s_ready) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const olog_outage_t *op = olog_index_find(&s_ix, outage);
    olog_outage_t o;
    if (op) o = *op;
    uint32_t end = s_next_slot;
    xSemaphoreGive(s_lock);
    if (!op) return false;

    char b[112];
    snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%u,"
             "\"cols\":[\"t\",\"charge\",\"load\",\"runtime\",\"vin\",\"vbat\",\"type\"],\"rows\":[",
             (unsigned)outage);
    emit(ctx, b);

    /* Its records follow its first one: only one outage is ever open, so
     * nothing else is written between them. Times as the index sums them,
     * seconds from the start; a restart of unknown length adds nothing. */
    uint32_t slot = o.first_slot, t = 0, rows = 0, last_up = 0, last_epoch = 0;
    uint16_t last_boot = 0;
    bool first = true;
    for (uint32_t i = 0; i < s_nslots && rows < MAX_SAMPLES; i++, slot = (slot + 1) % s_nslots) {
        if (!first && slot == end) break;
        const olog_rec_t *r = slot_rec(slot);
        /* A hole left by v0.16 can sit between an outage's records; past
         * the first record, the walk ends at `end` or the next outage. */
        if (olog_erased(r, OLOG_REC_SIZE) && first) break;
        if (!olog_valid(r)) continue;       /* a hole, torn, or overwritten as we read */
        if (r->outage != outage) {
            if (!first) break;
            continue;
        }
        if (first) {
            first = false;
        } else if (r->boot && r->boot == last_boot) {       /* 0: see nvs_load */
            if (r->uptime_s > last_up) t += r->uptime_s - last_up;
        } else if (r->epoch && last_epoch && r->epoch > last_epoch) {
            t += r->epoch - last_epoch;
        }
        last_up = r->uptime_s; last_epoch = r->epoch; last_boot = r->boot;

        int k = snprintf(b, sizeof(b), "%s[%u", rows ? "," : "", (unsigned)t);
        k += r->charge == OLOG_NONE8 ? snprintf(b + k, sizeof(b) - k, ",null")
                                     : snprintf(b + k, sizeof(b) - k, ",%u", r->charge);
        k += r->load == OLOG_NONE8 ? snprintf(b + k, sizeof(b) - k, ",null")
                                   : snprintf(b + k, sizeof(b) - k, ",%u", r->load);
        k += r->runtime_s == OLOG_NONE16 ? snprintf(b + k, sizeof(b) - k, ",null")
                                         : snprintf(b + k, sizeof(b) - k, ",%u", r->runtime_s);
        k += r->input_dv == OLOG_NONE16 ? snprintf(b + k, sizeof(b) - k, ",null")
                                        : snprintf(b + k, sizeof(b) - k, ",%.1f", r->input_dv / 10.0);
        k += r->batt_cv == OLOG_NONE16 ? snprintf(b + k, sizeof(b) - k, ",null")
                                       : snprintf(b + k, sizeof(b) - k, ",%.2f", r->batt_cv / 100.0);
        snprintf(b + k, sizeof(b) - k, ",%u]", r->type);
        emit(ctx, b);
        rows++;
        if (r->type == OLOG_END) break;
    }
    emit(ctx, "]}");
    return true;
}
