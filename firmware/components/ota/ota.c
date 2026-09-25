/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ota.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "netmgr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "ota";

/* The releases endpoint, not a branch: see the note in ota.h. */
#define GH_API "https://api.github.com/repos/aronreid/ups-to-esp32/releases/latest"

/* GitHub's release JSON is a few kilobytes and most of it is authorship and
 * URLs we do not want. Capped so a surprising response cannot exhaust the heap
 * on a device that is also running a USB host and a web server. */
#define JSON_MAX 16384

static ota_status_t     s_st;
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_q;
static char              s_url[256];
static volatile uint32_t s_requests;     /* HTTP requests completed this boot */

typedef enum { REQ_CHECK, REQ_INSTALL } req_t;

#define NVS_NS         "upsa"
#define NVS_KEY_AUTO   "ota_auto"

/* Long enough that GitHub's unauthenticated rate limit is irrelevant, short
 * enough that a security fix is noticed within a day. The first check waits a
 * minute after boot so it never competes with Wi-Fi association or the UPS
 * enumerating. */
#define CHECK_INTERVAL_MS   (12 * 60 * 60 * 1000)
#define FIRST_CHECK_MS      (60 * 1000)

/* What a freshly installed image must survive before the rollback is given up.
 * Long enough to cross the failures seen in practice -- a crash on the first
 * HTTP request, a watchdog a minute in, a brownout under Wi-Fi load -- and
 * short enough that a genuinely healthy device is not left one power cut away
 * from an undeserved downgrade. */
#define CONFIRM_MIN_UPTIME_S   300      /* five minutes on its feet          */
#define CONFIRM_MIN_REQUESTS   3        /* and actually answering for them   */

static void set_state(ota_state_t st, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = st;
    if (err) strlcpy(s_st.error, err, sizeof(s_st.error));
    else     s_st.error[0] = '\0';
    xSemaphoreGive(s_lock);
}

/* Fetch the latest release and pick out the asset for THIS board. */
static esp_err_t fetch_latest(char *tag, size_t tag_cap, char *url, size_t url_cap)
{
    esp_http_client_config_t cfg = {
        .url = GH_API,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
        /* Same reason as the download below. The API's headers happen to fit
         * in the 512-byte default, but only just, and a header GitHub adds
         * later would break checking silently. */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    /* GitHub rejects requests with no User-Agent. */
    esp_http_client_set_header(c, "User-Agent", "ups-adaptor");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "GitHub returned %d", status);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    char *body = malloc(JSON_MAX);
    if (!body) { esp_http_client_close(c); esp_http_client_cleanup(c); return ESP_ERR_NO_MEM; }
    int total = 0, r;
    while (total < JSON_MAX - 1 &&
           (r = esp_http_client_read(c, body + total, JSON_MAX - 1 - total)) > 0) {
        total += r;
    }
    body[total] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    err = ESP_ERR_NOT_FOUND;
    const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (cJSON_IsString(jtag)) {
        strlcpy(tag, jtag->valuestring, tag_cap);

        /* Exactly the asset for this target. A release that ships only the
         * other board's image is not an update for this one. */
        char want[64];
        snprintf(want, sizeof(want), "ups-adaptor-%s.bin", BOARD_OTA_TARGET);

        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
        const cJSON *a = NULL;
        cJSON_ArrayForEach(a, assets) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            const cJSON *dl   = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
            if (cJSON_IsString(name) && cJSON_IsString(dl) &&
                strcmp(name->valuestring, want) == 0) {
                strlcpy(url, dl->valuestring, url_cap);
                err = ESP_OK;
                break;
            }
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "release %s has no %s", tag, want);
    }
    cJSON_Delete(root);
    return err;
}

/* A release is stamped with its tag: v0.05, v0.06-rc1. Anything else is a
 * local build -- `git describe` falls back to a bare hash when no tag is an
 * ancestor, or appends -N-gHASH when commits follow one, and -dirty when the
 * tree had edits. Matches what release.yml publishes, not a general semver. */
static bool is_release_version(const char *v)
{
    if (v[0] != 'v' || !isdigit((unsigned char)v[1])) return false;
    return !strstr(v, "-g") && !strstr(v, "dirty");
}

static void do_check(bool asked)
{
    ota_state_t before;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    before = s_st.state;
    xSemaphoreGive(s_lock);

    if (asked) {
        /* An explicit check means "look again", including at a release this
         * board refused earlier -- see ota_status_t.refused. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.refused[0] = '\0';
        xSemaphoreGive(s_lock);
        set_state(OTA_CHECKING, NULL);
    }

    char tag[48] = "", url[256] = "";
    esp_err_t err = fetch_latest(tag, sizeof(tag), url, sizeof(url));
    if (err != ESP_OK) {
        /* A check nobody asked for fails quietly. Wi-Fi drops and GitHub has
         * outages, and waking up to a red error about neither is how people
         * learn to ignore the one that matters. */
        if (!asked) { set_state(before, NULL); return; }
        set_state(OTA_FAILED, err == ESP_ERR_NOT_FOUND
                  ? "no release with a binary for this board"
                  : "could not reach GitHub");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.latest, tag, sizeof(s_st.latest));
    strlcpy(s_url, url, sizeof(s_url));
    /* A plain string comparison, deliberately. Semantic ordering would let this
     * device decide a release is "older" and refuse it, and the most common
     * reason to publish one is to undo a bad one. Different means available. */
    bool differs = strcmp(s_st.running, tag) != 0;
    bool refused = differs && s_st.refused[0] && strcmp(s_st.refused, tag) == 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "running %s, latest %s%s", s_st.running, tag,
             refused ? "  -- refused earlier, not offering"
                     : differs ? "  -- UPDATE AVAILABLE" : "");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.checked_once = true;
    xSemaphoreGive(s_lock);
    if (refused) set_state(OTA_INCOMPATIBLE, "the published image is for a different board");
    else         set_state(differs ? OTA_AVAILABLE : OTA_UP_TO_DATE, NULL);
}

static void do_install(void)
{
    char url[256];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(url, s_url, sizeof(url));
    xSemaphoreGive(s_lock);

    if (!url[0]) { set_state(OTA_FAILED, "check for an update first"); return; }

    set_state(OTA_DOWNLOADING, NULL);
    ESP_LOGI(TAG, "installing %s", url);

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        /* Release assets redirect to objects.githubusercontent.com, so the
         * client must follow it and re-handshake against the new host. */
        .max_redirection_count = 5,
        /* The default header buffer is 512 bytes and GitHub's redirect does
         * not fit in it: the signed Location URL alone is several hundred, and
         * the whole header block is well past it. The symptom is
         * "HTTP_CLIENT: Out of buffer" followed by "Failed to open HTTP
         * connection", AFTER both certificates validate -- which makes it look
         * like a TLS or memory fault and is neither. */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) { set_state(OTA_FAILED, "download would not start"); return; }

    /* THE GATE THAT MATTERS. Read what the incoming image calls itself and
     * refuse anything that is not built for this board.
     *
     * The asset filename is only a label. A release once carried a Rev A build
     * under the devkit's filename -- the release workflow reused the first
     * target's sdkconfig for the second -- and a devkit installed it and came
     * up believing it had a load switch on GPIO4. Nothing downstream noticed,
     * because every layer trusted the name. The image's own descriptor cannot
     * be got wrong by a naming mistake, so that is what is checked, before a
     * single byte is committed to the other slot. */
    esp_app_desc_t incoming;
    if (esp_https_ota_get_img_desc(h, &incoming) != ESP_OK) {
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, "could not read the image's identity");
        return;
    }
    if (strncmp(incoming.project_name, BOARD_IMAGE_NAME,
                sizeof(incoming.project_name)) != 0) {
        ESP_LOGE(TAG, "REFUSED: image is \"%s\", this board runs \"%s\"",
                 incoming.project_name, BOARD_IMAGE_NAME);
        esp_https_ota_abort(h);
        char why[96];
        snprintf(why, sizeof(why), "built as \"%s\", this board runs \"%s\"",
                 incoming.project_name, BOARD_IMAGE_NAME);
        /* Remember the tag, so the periodic check stops putting an Install
         * button in front of someone for an image that cannot be installed.
         * The first version of this refused correctly and then offered it
         * again twelve hours later, which reads as a broken page rather than
         * a protected one. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_st.refused, s_st.latest, sizeof(s_st.refused));
        xSemaphoreGive(s_lock);
        set_state(OTA_INCOMPATIBLE, why);
        return;
    }
    ESP_LOGI(TAG, "image identity ok: %s %s",
             incoming.project_name, incoming.version);

    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.progress = (total > 0) ? (done * 100 / total) : 0;
        xSemaphoreGive(s_lock);
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, "the download did not complete");
        return;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        /* A signature or magic-byte failure lands here, which is the case this
         * exists to catch: refuse it rather than boot it. */
        set_state(OTA_FAILED, err == ESP_ERR_OTA_VALIDATE_FAILED
                  ? "the image failed validation and was not installed"
                  : "could not finish the install");
        return;
    }

    ESP_LOGW(TAG, "update installed; reboot to run it");
    set_state(OTA_INSTALLED, NULL);
}

static void ota_task(void *arg)
{
    TickType_t wait = pdMS_TO_TICKS(FIRST_CHECK_MS);

    for (;;) {
        req_t req;
        if (xQueueReceive(s_q, &req, wait) == pdTRUE) {
            if (req == REQ_CHECK) do_check(true);     /* a person asked */
            else                  do_install();
            wait = pdMS_TO_TICKS(CHECK_INTERVAL_MS);
            continue;
        }

        /* The timer fired. Only look if permitted and actually online -- and
         * never install, whatever it finds. */
        bool may;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        may = s_st.auto_check && s_st.state != OTA_INSTALLED
                              && s_st.state != OTA_DOWNLOADING;
        xSemaphoreGive(s_lock);

        if (may && netmgr_state() == NETMGR_STA_CONNECTED) do_check(false);
        wait = pdMS_TO_TICKS(CHECK_INTERVAL_MS);
    }
}

esp_err_t ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_q    = xQueueCreate(2, sizeof(req_t));
    if (!s_lock || !s_q) return ESP_ERR_NO_MEM;

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(s_st.running, app->version, sizeof(s_st.running));
    s_st.dev_build = !is_release_version(s_st.running);

    /* Default ON for CHECKING only, and the page says what that means. A
     * device that never looks can never tell its owner an update exists, and
     * the owner still has to press install. */
    s_st.auto_check = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_AUTO, &v) == ESP_OK) s_st.auto_check = v != 0;
        nvs_close(h);
    }

    /* If the running image is awaiting verification, this boot is the trial.
     * main confirms it once the device is demonstrably working. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
        s_st.pending_verify = (st == ESP_OTA_IMG_PENDING_VERIFY);
        if (s_st.pending_verify) {
            ESP_LOGW(TAG, "running a NEW image on trial; it rolls back unless confirmed");
        }
    }

    if (xTaskCreate(ota_task, "ota", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready, running %s for target %s", s_st.running, BOARD_OTA_TARGET);
    return ESP_OK;
}

void ota_get(ota_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

static esp_err_t enqueue(req_t r)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_q, &r, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ota_check(void)   { return enqueue(REQ_CHECK); }

esp_err_t ota_set_auto_check(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_AUTO, enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.auto_check = enabled;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "periodic update check %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}
esp_err_t ota_install(void) { return enqueue(REQ_INSTALL); }

void ota_note_request_served(void) { s_requests++; }

bool ota_confirm(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_st.pending_verify;
    xSemaphoreGive(s_lock);
    if (!pending) return true;      /* nothing on trial: nothing to earn */

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (up < CONFIRM_MIN_UPTIME_S || s_requests < CONFIRM_MIN_REQUESTS) {
        static uint32_t last_log;
        if (up - last_log >= 60) {           /* once a minute, not every tick */
            last_log = up;
            ESP_LOGI(TAG, "on trial: %lus of %ds, %lu of %d requests served",
                     (unsigned long)up, CONFIRM_MIN_UPTIME_S,
                     (unsigned long)s_requests, CONFIRM_MIN_REQUESTS);
        }
        return false;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "image confirmed good after %lus and %lu requests; "
                      "rollback cancelled", (unsigned long)up,
                 (unsigned long)s_requests);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.pending_verify = false;
        xSemaphoreGive(s_lock);
        return true;
    }
    return false;      /* esp_ota_mark_app_valid failed; try again next tick */
}
