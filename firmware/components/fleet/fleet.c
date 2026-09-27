/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fleet heartbeat. See fleet.h for what it is and the rules it keeps.
 */
#include "fleet.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "board.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netmgr.h"
#include "nut_server.h"
#include "nvs.h"
#include "ota.h"
#include "sdkconfig.h"
#include "ups_hid.h"
#include "crashlog.h"

static const char *TAG = "fleet";

#define NVS_NS        "upsa"
#define NVS_KEY_ON    "fl_on"
#define NVS_KEY_STATS "fl_stats"

/* A few times a day is enough to know a board is alive, and is nothing to a
 * free database. The server can change it in its reply, within these bounds. */
#define DEFAULT_INTERVAL_S  (6 * 60 * 60)
#define MIN_INTERVAL_S      (60 * 60)
#define MAX_INTERVAL_S      (24 * 60 * 60)
/* The first one waits until the board has settled: after Wi-Fi, the UPS
 * enumerating and the update check a minute in. Spread by MAC so a building
 * full of boards powering up together does not arrive in one second. */
#define FIRST_DELAY_S       180
#define FIRST_SPREAD_S      120
/* Failures back off from five minutes to the normal interval. */
#define RETRY_S             300
/* Skipped, not waited for: below these the board keeps its memory for its job.
 * A TLS handshake to the server needs roughly 40 KB in one piece. */
#define MIN_FREE_INTERNAL   (60 * 1024)
#define MIN_LARGEST_BLOCK   (40 * 1024)
#define HTTP_TIMEOUT_MS     10000
#define REPLY_MAX           512

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_kick;         /* settings changed: go now */
static fleet_status_t    s_st = { .last_ok_s = -1, .channel = "production" };
static char              s_mac[18];
/* Stats were withdrawn: one heartbeat without them is owed, even if the
 * heartbeat itself was switched off in the same breath, so the server lets go
 * of what it kept. */
static volatile bool     s_clear_owed;

static void set_error(const char *why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_error, why ? why : "", sizeof(s_st.last_error));
    xSemaphoreGive(s_lock);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default:               return "other";
    }
}

/* A float the UPS did not report is NaN; cJSON would print that as null
 * anyway, but leaving the key out keeps the row small. */
static void add_num(cJSON *o, const char *k, float v)
{
    if (!isnan(v)) cJSON_AddNumberToObject(o, k, v);
}

/* Whether this is one of the project's own test boards: the tier the fleet
 * server last gave it (kept in NVS by ota). */
static bool test_tier(void)
{
    ota_status_t o;
    ota_get(&o);
    return o.channel != OTA_CH_PRODUCTION;
}

/* Device diagnostics, for test boards only: what a developer needs to know
 * about the board itself when chasing a problem it reported. Includes its LAN
 * address and hostname, so the fleet site can link to its page -- which is
 * why production boards never send this. */
static cJSON *diag_object(void)
{
    cJSON *g = cJSON_CreateObject();
    if (!g) return NULL;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char s[72];
    snprintf(s, sizeof(s), "ESP32-S3 rev %d.%d, %d cores",
             ci.revision / 100, ci.revision % 100, ci.cores);
    cJSON_AddStringToObject(g, "chip", s);
    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK) cJSON_AddNumberToObject(g, "flash_mb", flash >> 20);
    cJSON_AddStringToObject(g, "idf", esp_get_idf_version());
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[17];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    cJSON_AddStringToObject(g, "elf", elf);
    snprintf(s, sizeof(s), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(g, "built", s);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) cJSON_AddStringToObject(g, "slot", run->label);

    cJSON_AddNumberToObject(g, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(g, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(g, "heap_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    nvs_handle_t h;
    uint32_t crashes = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "crashes", &crashes);          /* webui.c counts them */
        nvs_close(h);
    }
    cJSON_AddNumberToObject(g, "crashes", crashes);

    cJSON_AddStringToObject(g, "ip", netmgr_ip());
    cJSON_AddStringToObject(g, "host", netmgr_hostname());
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(g, "wifi_ch", ap.primary);

    ups_data_t d;
    ups_hid_get(&d);
    cJSON_AddNumberToObject(g, "ups_poll_failures", d.poll_failures);
    cJSON_AddBoolToObject(g, "vbus_fault", board_vbus_fault());

    ota_status_t o;
    ota_get(&o);
    cJSON_AddStringToObject(g, "ota_latest", o.latest);
    cJSON_AddNumberToObject(g, "ota_state", o.state);
    cJSON_AddBoolToObject(g, "ota_on_trial", o.pending_verify);
    if (o.error[0]) cJSON_AddStringToObject(g, "ota_error", o.error);
    return g;
}

/* The extras. Which UPS, how it is doing, how the board is doing -- opt-in on
 * production boards, automatic on test boards, which also add diagnostics.
 * Deliberately never the UPS serial or the Wi-Fi name. */
static cJSON *stats_object(bool diag)
{
    cJSON *s = cJSON_CreateObject();
    if (!s) return NULL;
    ups_data_t d;
    ups_hid_get(&d);
    cJSON_AddBoolToObject(s, "ups_attached", d.attached);
    if (d.attached) {
        char id[10];
        snprintf(id, sizeof(id), "%04x:%04x", d.vid, d.pid);
        cJSON_AddStringToObject(s, "ups_id", id);
        cJSON_AddStringToObject(s, "ups_mfr", d.mfr);
        cJSON_AddStringToObject(s, "ups_model", d.model);
        cJSON_AddNumberToObject(s, "ups_status", d.status);
        add_num(s, "battery_charge", d.battery_charge);
        add_num(s, "battery_runtime_s", d.battery_runtime);
        add_num(s, "load_pct", d.ups_load);
        add_num(s, "input_v", d.input_voltage);
        add_num(s, "realpower_nom_w", d.ups_realpower_nom);
        if (d.battery_mfr_date[0]) cJSON_AddStringToObject(s, "battery_date", d.battery_mfr_date);
    }
    cJSON_AddNumberToObject(s, "heap_low", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(s, "rssi", netmgr_rssi());
    cJSON_AddNumberToObject(s, "nut_clients", nut_server_clients());
    if (diag) {
        cJSON *g = diag_object();
        if (g) cJSON_AddItemToObject(s, "diag", g);
    }
    return s;
}

/* One heartbeat. Returns the interval the server asked for, or -1. */
static int send_heartbeat(bool with_stats, bool with_diag)
{
    cJSON *b = cJSON_CreateObject();
    if (!b) { set_error("out of memory"); return -1; }
    cJSON_AddStringToObject(b, "p_mac", s_mac);
    cJSON_AddStringToObject(b, "p_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(b, "p_board", BOARD_IMAGE_NAME);
    cJSON_AddNumberToObject(b, "p_uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(b, "p_reset_reason", reset_reason());
    /* Where the last crash happened, if there was one: code addresses and the
     * panic's own reason, nothing about the owner. */
    const char *crash = crashlog_last();
    cJSON_AddItemToObject(b, "p_crash", crash[0] ? cJSON_CreateString(crash) : cJSON_CreateNull());
    /* Explicit null when opted out: the server then clears what it kept. */
    cJSON *st = with_stats ? stats_object(with_diag) : NULL;
    cJSON_AddItemToObject(b, "p_stats", st ? st : cJSON_CreateNull());
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) { set_error("out of memory"); return -1; }

    esp_http_client_config_t cfg = {
        .url = CONFIG_UPSA_FLEET_URL "/rest/v1/rpc/heartbeat",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); set_error("out of memory"); return -1; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "User-Agent", "ups-esp32");
    esp_http_client_set_header(c, "apikey", CONFIG_UPSA_FLEET_KEY);
    /* Supabase's older anon keys are JWTs and go in Authorization as well; the
     * newer publishable keys are not, and are refused there. */
    if (strncmp(CONFIG_UPSA_FLEET_KEY, "sb_publishable_", 15) != 0) {
        esp_http_client_set_header(c, "Authorization", "Bearer " CONFIG_UPSA_FLEET_KEY);
    }

    int interval = -1;
    int len = strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK && esp_http_client_write(c, body, len) == len) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        char reply[REPLY_MAX];
        int got = 0, r;
        while (got < REPLY_MAX - 1 &&
               (r = esp_http_client_read(c, reply + got, REPLY_MAX - 1 - got)) > 0) {
            got += r;
        }
        reply[got] = '\0';
        if (status == 200) {
            cJSON *j = cJSON_Parse(reply);
            const cJSON *ch = j ? cJSON_GetObjectItemCaseSensitive(j, "channel") : NULL;
            const cJSON *iv = j ? cJSON_GetObjectItemCaseSensitive(j, "interval_s") : NULL;
            const cJSON *tg = j ? cJSON_GetObjectItemCaseSensitive(j, "target") : NULL;
            const cJSON *ai = j ? cJSON_GetObjectItemCaseSensitive(j, "auto_install") : NULL;
            const char *name = cJSON_IsString(ch) ? ch->valuestring : "production";
            ota_channel_t och = strcmp(name, "dev") == 0   ? OTA_CH_DEV
                              : strcmp(name, "debug") == 0 ? OTA_CH_DEBUG : OTA_CH_PRODUCTION;
            interval = cJSON_IsNumber(iv) ? iv->valueint : DEFAULT_INTERVAL_S;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s_st.channel, ota_channel_name(och), sizeof(s_st.channel));
            s_st.sent++;
            s_st.last_ok_s = esp_timer_get_time() / 1000000;
            s_st.last_error[0] = '\0';
            xSemaphoreGive(s_lock);
            ota_set_channel(och, cJSON_IsString(tg) ? tg->valuestring : "",
                            cJSON_IsTrue(ai));
            cJSON_Delete(j);
            /* A full HTTPS round trip: proof enough that a fresh image works. */
            ota_note_proven();
            ESP_LOGI(TAG, "heartbeat ok%s, channel %s", with_stats ? " with stats" : "",
                     ota_channel_name(och));
        } else {
            char why[48];
            snprintf(why, sizeof(why), "server answered %d", status);
            set_error(why);
            ESP_LOGW(TAG, "heartbeat refused: %d %.120s", status, reply);
        }
    } else {
        set_error("could not reach the server");
        ESP_LOGW(TAG, "heartbeat: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    return interval;
}

static void fleet_task(void *arg)
{
    /* Seconds until the next attempt, counted only while connected. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int32_t due = FIRST_DELAY_S + (mac[5] * 256 + mac[4]) % FIRST_SPREAD_S;
    int32_t interval = DEFAULT_INTERVAL_S;
    int fails = 0;

    for (;;) {
        /* A settings change cuts the wait short. */
        if (xSemaphoreTake(s_kick, pdMS_TO_TICKS(10 * 1000)) == pdTRUE) {
            if (due > 5) due = 5;
            continue;
        }
        fleet_status_t st;
        fleet_get(&st);
        if (!st.configured || !(st.enabled || s_clear_owed)) continue;
        if (netmgr_state() != NETMGR_STA_CONNECTED) continue;
        if ((due -= 10) > 0) continue;

        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MIN_FREE_INTERNAL ||
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < MIN_LARGEST_BLOCK) {
            set_error("deferred: memory is busy");
            due = RETRY_S;
            continue;
        }
        /* The update check or a download has the TLS gate: come back later
         * rather than queue behind it. */
        if (!ota_tls_take(0)) { due = 120; continue; }
        /* Test boards send stats and diagnostics whatever the box says. */
        bool auto_ = st.stats_auto && st.enabled;
        int got = send_heartbeat((st.stats || auto_) && st.enabled, auto_);
        ota_tls_give();
        if (got > 0) s_clear_owed = false;

        if (got > 0) {
            interval = got < MIN_INTERVAL_S ? MIN_INTERVAL_S
                     : got > MAX_INTERVAL_S ? MAX_INTERVAL_S : got;
            due = interval;
            fails = 0;
        } else {
            int32_t back = RETRY_S << (fails < 6 ? fails : 6);
            due = back < interval ? back : interval;
            fails++;
        }
    }
}

esp_err_t fleet_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_kick = xSemaphoreCreateBinary();
    if (!s_lock || !s_kick) return ESP_ERR_NO_MEM;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_st.configured = CONFIG_UPSA_FLEET_URL[0] && CONFIG_UPSA_FLEET_KEY[0];
    /* The heartbeat is on unless the owner turned it off; the extras are off
     * unless the owner turned them on. The page says so, next to both. */
    s_st.enabled = true;
    s_st.stats = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_ON, &v) == ESP_OK)    s_st.enabled = v != 0;
        if (nvs_get_u8(h, NVS_KEY_STATS, &v) == ESP_OK) s_st.stats = v != 0;
        nvs_close(h);
    }

    /* Priority 1: below every task that does the board's job. */
    if (xTaskCreate(fleet_task, "fleet", 6144, NULL, 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "%s; heartbeat %s, stats %s",
             s_st.configured ? "configured" : "no endpoint built in",
             s_st.enabled ? "on" : "off", s_st.stats ? "on" : "off");
    return ESP_OK;
}

void fleet_get(fleet_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
    out->stats_auto = test_tier();
}

esp_err_t fleet_set(bool enabled, bool stats)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_ON, enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_STATS, stats ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stats_dropped = s_st.stats && !stats;
    s_st.enabled = enabled;
    s_st.stats = stats;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "heartbeat %s, stats %s", enabled ? "on" : "off", stats ? "on" : "off");
    /* Send soon: to report on at once, or to clear stats the owner withdrew. */
    if (stats_dropped) s_clear_owed = true;
    if (enabled || stats_dropped) xSemaphoreGive(s_kick);
    return ESP_OK;
}
