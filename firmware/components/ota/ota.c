/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ota.h"
#include "ota_notes.h"
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

/* The releases endpoint, not a branch: see the note in ota.h. /latest skips
 * prereleases; the list, newest first, includes them -- one item is enough. */
#define GH_REPO   "https://api.github.com/repos/aronreid/ups-to-esp32/releases"
#define GH_API    GH_REPO "/latest"
#define GH_NEWEST GH_REPO "?per_page=1"
#define GH_TAG    GH_REPO "/tags/"

/* GitHub's release JSON is a few kilobytes and most of it is authorship and
 * URLs we do not want. Capped so a surprising response cannot exhaust the heap
 * on a device that is also running a USB host and a web server.
 *
 * A response that does NOT fit fails to parse at all -- cJSON has no partial
 * mode -- and the release notes ride in the same object, last. So the cap has
 * room to spare, an overflow is reported as itself, and release.yml refuses
 * notes long enough to threaten it. Boards on v0.06 and earlier cap at 16 KB:
 * the release job keeps the whole response well under that, for them. */
#define JSON_MAX 24576

/* The release's "What changed" bullets, abridged for the update card, and
 * where the full notes are. Filled by each successful check. */
static char s_notes[NOTES_MAX_ITEMS * (NOTES_ITEM_CHARS + 1) + 1];
static char s_notes_url[160];
static char s_notes_tag[48];

static ota_status_t     s_st;
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_q;
static char              s_url[256];
static volatile uint32_t s_requests;     /* HTTP requests completed this boot */
static SemaphoreHandle_t s_tls;          /* see ota_tls_take() */
static volatile bool     s_reboot_due;   /* an automatic install finished */
/* When the fleet server last confirmed this board's channel, this boot. An
 * automatic install needs it recent: a board whose server is unreachable --
 * no internet, or the heartbeat blocked on an IoT network -- must not keep
 * acting on a tier it may since have been taken off. Unheard, it only offers,
 * like any production board. */
static volatile int64_t  s_channel_heard_us = -1;
#define CHANNEL_FRESH_US    (48LL * 60 * 60 * 1000000)
static void do_install(void);

typedef enum { REQ_CHECK, REQ_INSTALL } req_t;

#define NVS_NS         "upsa"
#define NVS_KEY_AUTO   "ota_auto"
#define NVS_KEY_RC     "ota_rc"      /* v0.09-rc1's on/off; read once to migrate */
#define NVS_KEY_CH     "ota_ch"
#define NVS_KEY_TGT    "ota_tgt"
#define NVS_KEY_AI     "ota_ai"
#define NVS_KEY_TRY    "ota_try"     /* the release last auto-installed ... */
#define NVS_KEY_TRIES  "ota_tries"   /* ... and how many times it was tried */
#define AUTO_TRIES_MAX 2

/* Long enough that GitHub's unauthenticated rate limit is irrelevant, short
 * enough that a security fix is noticed within a day. The first check waits a
 * minute after boot so it never competes with Wi-Fi association or the UPS
 * enumerating. */
#define CHECK_INTERVAL_MS   (12 * 60 * 60 * 1000)
/* Dev and debug boards are kept current: a new build reaches them the same day. */
#define AUTO_INTERVAL_MS    (3 * 60 * 60 * 1000)
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
    ota_channel_t ch;
    char target[32];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ch = s_st.channel;
    strlcpy(target, s_st.target, sizeof(target));
    xSemaphoreGive(s_lock);

    /* production: the latest full release. debug: the named release, or the
     * latest full one if none is named yet. dev: the named release, or the
     * newest of any kind. */
    char api[160];
    if (ch != OTA_CH_PRODUCTION && target[0]) snprintf(api, sizeof(api), GH_TAG "%s", target);
    else strlcpy(api, ch == OTA_CH_DEV ? GH_NEWEST : GH_API, sizeof(api));

    esp_http_client_config_t cfg = {
        .url = api,
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
    esp_http_client_set_header(c, "User-Agent", "ups-esp32");
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
    bool truncated = total >= JSON_MAX - 1 && esp_http_client_read(c, &(char){0}, 1) > 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (truncated) {
        ESP_LOGE(TAG, "release info is larger than %d bytes; cannot read it", JSON_MAX);
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    /* The release-candidate endpoint answers with a list of one. */
    const cJSON *rel = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root;
    if (!rel) { cJSON_Delete(root); return ESP_ERR_NOT_FOUND; }

    err = ESP_ERR_NOT_FOUND;
    const cJSON *jtag = cJSON_GetObjectItemCaseSensitive(rel, "tag_name");
    if (cJSON_IsString(jtag)) {
        strlcpy(tag, jtag->valuestring, tag_cap);

        const cJSON *jbody = cJSON_GetObjectItemCaseSensitive(rel, "body");
        const cJSON *jhtml = cJSON_GetObjectItemCaseSensitive(rel, "html_url");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ota_abridge_notes(cJSON_IsString(jbody) ? jbody->valuestring : NULL,
                      s_notes, sizeof(s_notes));
        strlcpy(s_notes_url, cJSON_IsString(jhtml) ? jhtml->valuestring : "",
                sizeof(s_notes_url));
        strlcpy(s_notes_tag, tag, sizeof(s_notes_tag));
        xSemaphoreGive(s_lock);

        /* Exactly the asset for this target. A release that ships only the
         * other board's image is not an update for this one. */
        char want[64];
        snprintf(want, sizeof(want), "ups-adaptor-%s.bin", BOARD_OTA_TARGET);

        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(rel, "assets");
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

/* Dev and debug boards take what their channel offers without waiting for a
 * person. Not over a local build (a bench board being tested by hand), not
 * while a fresh image is still on trial, and at most AUTO_TRIES_MAX times for
 * one release: an image that rolled back, or will not download, is left
 * offered on the page rather than retried forever. */
static void maybe_auto_install(const char *tag)
{
    bool go;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    go = s_st.auto_install && s_st.channel != OTA_CH_PRODUCTION &&
         !s_st.dev_build && !s_st.pending_verify;
    xSemaphoreGive(s_lock);
    if (!go) return;
    if (s_channel_heard_us < 0 ||
        esp_timer_get_time() - s_channel_heard_us > CHANNEL_FRESH_US) {
        ESP_LOGW(TAG, "%s offered, not installed: the fleet server has not confirmed "
                      "this board's channel recently", tag);
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char tried[48] = "";
    size_t len = sizeof(tried);
    uint8_t tries = 0;
    if (nvs_get_str(h, NVS_KEY_TRY, tried, &len) == ESP_OK && strcmp(tried, tag) == 0) {
        nvs_get_u8(h, NVS_KEY_TRIES, &tries);
    }
    if (tries >= AUTO_TRIES_MAX) {
        nvs_close(h);
        ESP_LOGW(TAG, "%s already tried %u times; offering it instead", tag, tries);
        return;
    }
    nvs_set_str(h, NVS_KEY_TRY, tag);
    nvs_set_u8(h, NVS_KEY_TRIES, tries + 1);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "installing %s automatically (%s channel, attempt %u)",
             tag, ota_channel_name(s_st.channel), tries + 1);
    do_install();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reboot_due = s_st.state == OTA_INSTALLED;
    xSemaphoreGive(s_lock);
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
    /* Waits for a heartbeat in flight; those last seconds, not minutes. */
    ota_tls_take(UINT32_MAX);
    esp_err_t err = fetch_latest(tag, sizeof(tag), url, sizeof(url));
    ota_tls_give();
    if (err != ESP_OK) {
        /* A check nobody asked for fails quietly. Wi-Fi drops and GitHub has
         * outages, and waking up to a red error about neither is how people
         * learn to ignore the one that matters. */
        if (!asked) { set_state(before, NULL); return; }
        set_state(OTA_FAILED, err == ESP_ERR_NOT_FOUND
                  ? "no release with a binary for this board"
                  : err == ESP_ERR_INVALID_SIZE
                  ? "the release information is too large to read"
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
    if (!refused && differs) maybe_auto_install(tag);
}

static void do_install_gated(void)
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

/* The download holds the TLS gate throughout: a heartbeat waits for it. */
static void do_install(void)
{
    ota_tls_take(UINT32_MAX);
    do_install_gated();
    ota_tls_give();
}

bool ota_tls_take(uint32_t wait_ms)
{
    return xSemaphoreTake(s_tls, wait_ms == UINT32_MAX ? portMAX_DELAY
                                                       : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void ota_tls_give(void)
{
    xSemaphoreGive(s_tls);
}

static void ota_task(void *arg)
{
    TickType_t wait = pdMS_TO_TICKS(FIRST_CHECK_MS);

    for (;;) {
        req_t req;
        if (xQueueReceive(s_q, &req, wait) == pdTRUE) {
            if (req == REQ_CHECK) do_check(true);     /* a person asked */
            else                  do_install();
        } else {
            /* The timer fired. Only look if permitted and actually online. */
            bool may;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            /* A dev or debug board checks whatever "check daily" says: the
             * project put it on that channel to be kept current. */
            may = (s_st.auto_check || s_st.auto_install)
                  && s_st.state != OTA_INSTALLED && s_st.state != OTA_DOWNLOADING;
            xSemaphoreGive(s_lock);
            if (may && netmgr_state() == NETMGR_STA_CONNECTED) do_check(false);
        }
        wait = pdMS_TO_TICKS(s_st.auto_install ? AUTO_INTERVAL_MS : CHECK_INTERVAL_MS);
        /* An automatic install runs the new image at once: nobody is there to
         * press Restart. It goes on trial and rolls back by itself if bad. */
        if (s_reboot_due) {
            ESP_LOGW(TAG, "restarting into the new image");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
    }
}

esp_err_t ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_q    = xQueueCreate(2, sizeof(req_t));
    s_tls  = xSemaphoreCreateMutex();
    if (!s_lock || !s_q || !s_tls) return ESP_ERR_NO_MEM;

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
        if (nvs_get_u8(h, NVS_KEY_CH, &v) == ESP_OK && v <= OTA_CH_DEV) {
            s_st.channel = (ota_channel_t)v;
        } else if (nvs_get_u8(h, NVS_KEY_RC, &v) == ESP_OK && v) {
            s_st.channel = OTA_CH_DEV;          /* v0.09-rc1 called this "rc" */
        }
        size_t len = sizeof(s_st.target);
        if (nvs_get_str(h, NVS_KEY_TGT, s_st.target, &len) != ESP_OK) s_st.target[0] = '\0';
        if (nvs_get_u8(h, NVS_KEY_AI, &v) == ESP_OK) s_st.auto_install = v != 0;
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

void ota_get_notes(char *tag, size_t tag_cap, char *url, size_t url_cap,
                   char *notes, size_t notes_cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(tag, s_notes_tag, tag_cap);
    strlcpy(url, s_notes_url, url_cap);
    strlcpy(notes, s_notes, notes_cap);
    xSemaphoreGive(s_lock);
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

const char *ota_channel_name(ota_channel_t ch)
{
    return ch == OTA_CH_DEV ? "dev" : ch == OTA_CH_DEBUG ? "debug" : "production";
}

/* A tag as release.yml makes them: v, a digit, then letters, digits, dots and
 * dashes. Anything else from the server is ignored rather than put in a URL. */
static bool tag_ok(const char *t)
{
    if (!t || t[0] != 'v' || !isdigit((unsigned char)t[1]) || strlen(t) > 31) return false;
    for (const char *p = t; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false;
    }
    return true;
}

esp_err_t ota_set_channel(ota_channel_t ch, const char *target, bool auto_install)
{
    if (ch > OTA_CH_DEV) ch = OTA_CH_PRODUCTION;
    char tgt[32] = "";
    if (target && target[0] && tag_ok(target)) strlcpy(tgt, target, sizeof(tgt));
    /* Production never installs by itself, whatever the server says. */
    if (ch == OTA_CH_PRODUCTION) auto_install = false;
    s_channel_heard_us = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_st.channel == ch && strcmp(s_st.target, tgt) == 0 &&
                s_st.auto_install == auto_install;
    xSemaphoreGive(s_lock);
    /* Called on every heartbeat: flash is written only when it changes. */
    if (same) return ESP_OK;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_CH, (uint8_t)ch);
    nvs_set_str(h, NVS_KEY_TGT, tgt);
    nvs_set_u8(h, NVS_KEY_AI, auto_install ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.channel = ch;
    strlcpy(s_st.target, tgt, sizeof(s_st.target));
    s_st.auto_install = auto_install;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "update channel %s%s%s, %s", ota_channel_name(ch),
             tgt[0] ? " -> " : "", tgt, auto_install ? "installs automatically" : "offers only");
    /* Look again now rather than in up to twelve hours. */
    return enqueue(REQ_CHECK);
}

void ota_note_proven(void)
{
    if (s_requests < CONFIRM_MIN_REQUESTS) s_requests = CONFIRM_MIN_REQUESTS;
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
