/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tailscale supervisor. See tailnet.h for what it promises and README.md for
 * how it is built and configured.
 *
 * One task owns the MicroLink instance: it is the only code that creates,
 * rebinds or destroys it. Event handlers only set bits, because
 * microlink_stop() blocks for up to fifteen seconds while its tasks wind
 * down, and doing that inside the default event loop would stall netmgr's own
 * reconnect handling -- the one thing on this board that must never stall.
 */
#include "tailnet.h"
#include "netmgr.h"
#include "microlink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <ctype.h>

#if CONFIG_ML_ENABLE_CONFIG_HTTPD
/* It binds port 80, which the web UI owns, and serves an unauthenticated page
 * that rewrites Wi-Fi and the auth key. Kconfig hides it; this catches an
 * sdkconfig edited by hand. */
#error "MicroLink's config HTTP server must stay off: the web UI owns port 80"
#endif

static const char *TAG = "tailnet";

/* netmgr's namespace, so a factory reset (BOOT pressed five times) clears the
 * key along with Wi-Fi: a board handed on must not stay on its old owner's
 * tailnet. */
#define NVS_NS      "upsa"
#define NVS_KEY_KEY "ts_key"
#define NVS_KEY_ON  "ts_on"

/* Tailscale auth keys run to about 60 characters; the headroom is for
 * whatever format comes next, not for accepting junk. */
#define KEY_MAX 128

#define EV_GOT_IP   BIT0
#define EV_LOST     BIT1
#define EV_RECONFIG BIT2

/* How long MicroLink may sit short of CONNECTED, with Wi-Fi up, before it is
 * torn down and built again. Its own coordinator already retries forever with
 * backoff; this is for the case that retrying inside one instance cannot fix
 * -- a wedged task or a socket in a state nothing resets. Long, because a
 * rebuild throws away working DERP and peer state, and doubling after each
 * one so a bad key does not churn the board every few minutes all day. */
#define REBUILD_MIN_US  ((int64_t)10 * 60 * 1000000)
#define REBUILD_MAX_US  ((int64_t)60 * 60 * 1000000)
/* After a failed microlink_init()/start(), usually no PSRAM or no NVS. */
#define START_RETRY_US  ((int64_t)60 * 1000000)
#define TICK_MS         5000

/* The web server starts before tailnet_start() (main.c), so tailnet_get()
 * and tailnet_configure() can be called while these are still NULL; both
 * check. They are created from static storage, so a low-memory boot cannot
 * leave them NULL for good. */
static StaticEventGroup_t s_ev_buf;
static StaticSemaphore_t s_lock_buf;
static EventGroupHandle_t s_ev;
/* Guards s_ml against the web UI reading it while the task tears it down.
 * Held only for pointer swaps and short reads, never across a stop. */
static SemaphoreHandle_t s_lock;
static microlink_t *s_ml;

/* Settings. Written by tailnet_configure() under s_lock, read by the task. */
static bool s_enabled;
static char s_key[KEY_MAX + 1];
static bool s_forget_pending;

static tailnet_state_t s_state = TAILNET_OFF;

const char *tailnet_state_name(tailnet_state_t s)
{
    switch (s) {
    case TAILNET_OFF:        return "off";
    case TAILNET_NO_KEY:     return "no key";
    case TAILNET_WAITING:    return "waiting for Wi-Fi";
    case TAILNET_CONNECTING: return "connecting";
    case TAILNET_CONNECTED:  return "connected";
    case TAILNET_ERROR:      return "error";
    }
    return "?";
}

static void load_settings(void)
{
    nvs_handle_t h;
    s_enabled = false;
    s_key[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    size_t len = sizeof(s_key);
    nvs_get_u8(h, NVS_KEY_ON, &on);
    if (nvs_get_str(h, NVS_KEY_KEY, s_key, &len) != ESP_OK) s_key[0] = '\0';
    nvs_close(h);
    s_enabled = on != 0;
}

/* A Tailscale auth key: "tskey-" and then letters, digits and dashes. Checked
 * so a stray paste -- an API key, a URL, a key with a trailing newline -- is
 * refused at the form rather than failing silently at the control plane. */
static bool key_plausible(const char *k)
{
    size_t n = strlen(k);
    if (n < 16 || n > KEY_MAX || strncmp(k, "tskey-", 6) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)k[i]) && k[i] != '-' && k[i] != '_') return false;
    }
    return true;
}

esp_err_t tailnet_configure(bool enabled, const char *key, bool forget)
{
    if (!forget && key && key[0] && !key_plausible(key)) return ESP_ERR_INVALID_ARG;
    /* Not started yet: refuse rather than store a change the task would not
     * be told about (the page says "could not save" and can be retried). */
    if (!s_lock || !s_ev) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (forget) {
        nvs_erase_key(h, NVS_KEY_KEY);
        enabled = false;
    } else if (key && key[0]) {
        nvs_set_str(h, NVS_KEY_KEY, key);
    }
    nvs_set_u8(h, NVS_KEY_ON, enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = enabled;
    if (forget) {
        s_key[0] = '\0';
        s_forget_pending = true;
    } else if (key && key[0]) {
        strlcpy(s_key, key, sizeof(s_key));
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "settings changed: %s%s", enabled ? "on" : "off",
             forget ? ", key and identity forgotten" : "");
    xEventGroupSetBits(s_ev, EV_RECONFIG);
    return ESP_OK;
}

void tailnet_get(tailnet_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) {                   /* before tailnet_start(): report off */
        out->state = TAILNET_OFF;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_state;
    out->enabled = s_enabled;
    out->key_set = s_key[0] != '\0';
    if (s_ml && s_state == TAILNET_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(s_ml);
        if (ip) microlink_ip_to_str(ip, out->ip);
        out->peers = microlink_get_peer_count(s_ml);
    }
    xSemaphoreGive(s_lock);
}

/* Event handlers: record and return. See the note at the top of the file. */
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_ev, EV_LOST);
    }
}

static void set_state(tailnet_state_t st)
{
    if (st == s_state) return;
    ESP_LOGI(TAG, "%s -> %s", tailnet_state_name(s_state), tailnet_state_name(st));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_lock);
}

/* Detach under the lock, stop outside it. microlink_destroy() stops first and
 * waits for the tasks; if one will not exit it leaks the instance on purpose
 * rather than free memory under a running task -- a leak in PSRAM, logged,
 * against a use-after-free panic. */
static void teardown(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    microlink_t *ml = s_ml;
    s_ml = NULL;
    xSemaphoreGive(s_lock);
    if (!ml) return;
    ESP_LOGI(TAG, "stopping Tailscale");
    microlink_destroy(ml);
}

static bool bring_up(void)
{
    /* microlink_init() copies the config struct but not the strings it points
     * at, and reads the key later, from its coordinator task, when it
     * registers. So the key it is given has to outlive this call and must not
     * change under it: a copy only this task writes, and only between
     * instances -- teardown() has always run before this. */
    static char key_live[KEY_MAX + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(key_live, s_key, sizeof(key_live));
    xSemaphoreGive(s_lock);

    /* The board's own name, so the tailnet's machine list, the router's
     * client list and the setup AP all show the same ups-esp32-XXXX.
     * netmgr_hostname() is static storage. */
    microlink_config_t cfg = {
        .auth_key = key_live,
        .device_name = netmgr_hostname(),
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 0,              /* 0: CONFIG_ML_MAX_PEERS */
    };

    microlink_t *ml = microlink_init(&cfg);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        return false;
    }
    if (microlink_start(ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
        microlink_destroy(ml);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ml = ml;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Tailscale starting as %s", netmgr_hostname());
    return true;
}

static void tailnet_task(void *arg)
{
    int64_t started_us = 0;          /* when this instance last started or rebound */
    int64_t rebuild_after_us = REBUILD_MIN_US;
    int64_t retry_at_us = 0;         /* after a failed start */
    bool was_connected = false;

    for (;;) {
        EventBits_t ev = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_LOST | EV_RECONFIG,
                                             pdTRUE, pdFALSE, pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (ev & EV_RECONFIG) {
            /* Any change of key or switch: start from nothing, so a new key is
             * never mixed with an instance registered under the old one. */
            teardown();
            bool forget;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            forget = s_forget_pending;
            s_forget_pending = false;
            xSemaphoreGive(s_lock);
            if (forget) {
                /* Node and machine keys too, so the next key registers a new
                 * machine instead of reviving the old one on another tailnet. */
                microlink_factory_reset();
            }
            retry_at_us = 0;
            rebuild_after_us = REBUILD_MIN_US;
            was_connected = false;
        }

        bool enabled, have_key;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        enabled = s_enabled;
        have_key = s_key[0] != '\0';
        xSemaphoreGive(s_lock);

        if (!enabled || !have_key) {
            teardown();
            set_state(enabled ? TAILNET_NO_KEY : TAILNET_OFF);
            continue;
        }

        /* netmgr is the one source of truth for "on the network". */
        bool online = netmgr_state() == NETMGR_STA_CONNECTED;

        if (!s_ml) {
            if (!online) { set_state(TAILNET_WAITING); continue; }
            if (now < retry_at_us) continue;
            if (bring_up()) {
                started_us = now;
                set_state(TAILNET_CONNECTING);
            } else {
                retry_at_us = now + START_RETRY_US;
                set_state(TAILNET_ERROR);
            }
            continue;
        }

        /* A Wi-Fi dropout is left to MicroLink: its sockets fail and its
         * coordinator backs off and retries by itself. Tearing it down here
         * would cost a full re-registration for every router blip. When the
         * address comes back -- possibly a different one -- the control and
         * DERP links are forced to reconnect now instead of whenever their
         * backoff next wakes, which is what gets the tailnet address
         * answering again within seconds of Wi-Fi. */
        if ((ev & EV_GOT_IP) && online) {
            ESP_LOGI(TAG, "Wi-Fi is back, rebinding");
            if (microlink_rebind(s_ml) != ESP_OK) {
                teardown();              /* rebuilt on the next pass */
                continue;
            }
            started_us = now;
        }

        microlink_state_t ms = microlink_get_state(s_ml);
        bool connected = ms == ML_STATE_CONNECTED;
        if (connected && !was_connected) {
            tailnet_status_t st;
            set_state(TAILNET_CONNECTED);
            tailnet_get(&st);
            ESP_LOGI(TAG, "on the tailnet as %s", st.ip[0] ? st.ip : "(no address yet)");
            rebuild_after_us = REBUILD_MIN_US;
        } else if (!connected) {
            set_state(online ? TAILNET_CONNECTING : TAILNET_WAITING);
        }
        if (connected) started_us = now;   /* measures time spent NOT connected */
        was_connected = connected;

        /* Down too long with Wi-Fi up: rebuild. Wi-Fi being down is not
         * Tailscale's fault and is netmgr's to fix, so it does not count. */
        if (!connected && online && now - started_us > rebuild_after_us) {
            ESP_LOGW(TAG, "not on the tailnet for %llds with Wi-Fi up, rebuilding",
                     (long long)((now - started_us) / 1000000));
            teardown();
            rebuild_after_us = rebuild_after_us * 2 > REBUILD_MAX_US
                             ? REBUILD_MAX_US : rebuild_after_us * 2;
        } else if (!online) {
            started_us = now;
        }
    }
}

esp_err_t tailnet_start(void)
{
    if (s_lock) return ESP_ERR_INVALID_STATE;     /* already started */
    /* Settings first, then the handles: once s_lock is visible the web UI
     * may read s_enabled and s_key. */
    load_settings();
    s_ev = xEventGroupCreateStatic(&s_ev_buf);
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    ESP_LOGI(TAG, "Tailscale %s%s", s_enabled ? "on" : "off",
             s_enabled && !s_key[0] ? ", but no auth key is stored" : "");

    esp_err_t err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL, NULL);
    }
    if (err != ESP_OK) return err;

    /* Most of MicroLink runs in its own tasks, but microlink_init() generates
     * three Curve25519 key pairs on the first run, and microlink_destroy()
     * frees TLS state, both on this stack. 6 KB is a guess with margin, not a
     * measurement: CONFIG_UPSA_MEM_DIAG reports what it really uses. */
    if (xTaskCreate(tailnet_task, "tailnet", 6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
