/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MQTT on the board: settings in NVS, IDF's esp-mqtt client, and one task
 * that publishes. Topics and payloads are in mqtt_core.c.
 *
 * The client's own task receives (commands, Home Assistant's birth message)
 * and only sets flags; this component's task does every publish, once a
 * second at most: discovery after each connection and each Home Assistant
 * restart, the outputs whenever they change, and the readings every 30
 * seconds or at once when the UPS's status changes. Neither task exists until
 * the owner turns MQTT on, so a board that never uses it keeps the RAM.
 */
#include "mqttlink.h"
#include "mqtt_core.h"
#include "outputs.h"
#include "ups_hid.h"
#include "netmgr.h"
#include "board.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mqtt";

#define NVS_NS        "upsa"       /* netmgr_factory_reset() erases it */
#define TASK_STACK    4096
#define UPS_EVERY_US  (30LL * 1000 * 1000)

#define F_DISCOVERY   (1u << 0)
#define F_OUTPUTS     (1u << 1)
#define F_UPS         (1u << 2)
#define F_RESTART     (1u << 3)

static SemaphoreHandle_t s_lock;       /* s_cfg, s_status */
static mqtt_cfg_t        s_cfg;
static mqttlink_status_t s_status;
static TaskHandle_t      s_task;
static esp_mqtt_client_handle_t s_client;   /* the task's */
static uint32_t          s_flags;      /* F_*, atomically */
static volatile bool     s_connected;
static bool              s_control;    /* MQTT could operate outputs when we subscribed */

static char s_base[MQTT_BASE_MAX];
static char s_id[32];

/* Buffers, allocated with the task. */
typedef struct {
    char topic[128];
    char body[1024];
    char ups_last[512];
} ctx_t;
static ctx_t *s_ctx;

/* ---- settings ------------------------------------------------------------ */

static void nvs_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
}

static void load(mqtt_cfg_t *c)
{
    mqtt_cfg_defaults(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "mqtt_on", &u8) == ESP_OK) c->enabled = u8 != 0;
    if (nvs_get_u8(h, "mqtt_disc", &u8) == ESP_OK) c->discovery = u8 != 0;
    if (nvs_get_u16(h, "mqtt_port", &u16) == ESP_OK && u16) c->port = u16;
    nvs_str(h, "mqtt_host", c->host, sizeof(c->host));
    nvs_str(h, "mqtt_user", c->user, sizeof(c->user));
    nvs_str(h, "mqtt_pass", c->pass, sizeof(c->pass));
    nvs_close(h);
    if (!mqtt_cfg_check(c, NULL, 0)) {
        ESP_LOGW(TAG, "saved settings do not check out; MQTT stays off");
        c->enabled = false;
    }
}

static esp_err_t save(const mqtt_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(h, "mqtt_on", c->enabled);
    if (err == ESP_OK) err = nvs_set_u8(h, "mqtt_disc", c->discovery);
    if (err == ESP_OK) err = nvs_set_u16(h, "mqtt_port", c->port);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_host", c->host);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_user", c->user);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_pass", c->pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void mqttlink_get_cfg(mqtt_cfg_t *out)
{
    if (!s_lock) { mqtt_cfg_defaults(out); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void mqttlink_get_status(mqttlink_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    out->enabled = s_cfg.enabled;
    out->connected = s_connected;
    strlcpy(out->base, s_base, sizeof(out->base));
    xSemaphoreGive(s_lock);
}

static void set_error(const char *e)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.last_error, e, sizeof(s_status.last_error));
    xSemaphoreGive(s_lock);
}

static void flag(uint32_t f)
{
    __atomic_fetch_or(&s_flags, f, __ATOMIC_SEQ_CST);
    if (s_task) xTaskNotifyGive(s_task);
}

/* Outputs changed (any source): publish their state, and discovery again if
 * a setting changed what they are. Cheap, as the listener must be. */
static void outputs_changed(void)
{
    /* Discovery only when a setting changed: sent with every switch and
     * press, some twenty retained messages went out ahead of the state. */
    static uint32_t seen;
    uint32_t g = outputs_cfg_gen();
    bool cfg = __atomic_exchange_n(&seen, g, __ATOMIC_SEQ_CST) != g;
    flag(cfg ? F_OUTPUTS | F_DISCOVERY : F_OUTPUTS);
}

/* ---- the client ---------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.connects++;
        s_status.last_error[0] = '\0';
        xSemaphoreGive(s_lock);
        char t[MQTT_BASE_MAX + 24];
        esp_mqtt_client_subscribe(ev->client, MQTT_DISC_PREFIX "/status", 0);
        if (s_control) {
            snprintf(t, sizeof(t), "%s/output/+/set", s_base);
            esp_mqtt_client_subscribe(ev->client, t, 0);
            snprintf(t, sizeof(t), "%s/output/+/press", s_base);
            esp_mqtt_client_subscribe(ev->client, t, 0);
        }
        ESP_LOGI(TAG, "connected to the broker as %s", s_base);
        flag(F_DISCOVERY | F_OUTPUTS | F_UPS);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected from the broker");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) break;
        int idx = 0;
        bool on = false;
        mqtt_cmd_t c = mqtt_parse(s_base, ev->topic, ev->topic_len, ev->data, ev->data_len, &idx, &on);
        if (c == MQTT_CMD_HA_ONLINE) { flag(F_DISCOVERY | F_OUTPUTS | F_UPS); break; }
        if (c == MQTT_CMD_NONE) break;
        /* A RETAINED command is not a command. The broker hands a retained
         * message to every new subscription, so one published with the
         * retain flag would run again after every restart, update, Wi-Fi
         * drop and broker restart: a Button pressing itself, a Switch
         * turned off on the page coming back on. Home Assistant's own
         * entities never retain commands. */
        if (ev->retain) {
            ESP_LOGW(TAG, "output %d: a retained command is ignored (clear it on the broker)", idx + 1);
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.commands++;
        xSemaphoreGive(s_lock);
        char why[96] = "";
        esp_err_t err = c == MQTT_CMD_SET ? outputs_set(idx, on, OUT_SRC_MQTT, why, sizeof(why))
                                          : outputs_press(idx, OUT_SRC_MQTT, why, sizeof(why));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "output %d: %s", idx + 1, why);
            flag(F_OUTPUTS);   /* put Home Assistant's switch back where it is */
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            char e[64];
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                int rc = ev->error_handle->connect_return_code;
                snprintf(e, sizeof(e), rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                       rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                                       ? "the broker refused the user name or password"
                                       : "the broker refused the connection (code %d)", rc);
            } else {
                snprintf(e, sizeof(e), "could not reach the broker");
            }
            set_error(e);
        }
        break;
    default:
        break;
    }
}

static void client_stop(void)
{
    if (!s_client) return;
    /* A clean disconnect sends no will: say offline ourselves. */
    if (s_connected) {
        char t[MQTT_BASE_MAX + 8];
        snprintf(t, sizeof(t), "%s/status", s_base);
        esp_mqtt_client_publish(s_client, t, "offline", 7, 1, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_mqtt_client_destroy(s_client);   /* stops its task */
    s_client = NULL;
    s_connected = false;
}

static void client_start(void)
{
    mqtt_cfg_t c;
    mqttlink_get_cfg(&c);
    if (!c.enabled) return;
    out_cfg_t oc;
    outputs_get_cfg(&oc);
    s_control = oc.mqtt_control;
    char will[MQTT_BASE_MAX + 8];
    snprintf(will, sizeof(will), "%s/status", s_base);
    esp_mqtt_client_config_t mc = {
        .broker.address.hostname = c.host,
        .broker.address.port = c.port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = s_id,
        .credentials.username = c.user[0] ? c.user : NULL,
        .credentials.authentication.password = c.pass[0] ? c.pass : NULL,
        .session.last_will.topic = will,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
        .buffer.size = 512,
        .buffer.out_size = 1280,
        /* Commands run on this task and save the Switch states (NVS). */
        .task.stack_size = 6144,
        .task.priority = 3,
    };
    s_client = esp_mqtt_client_init(&mc);   /* copies every string */
    memset(&c, 0, sizeof(c));
    if (!s_client) { set_error("out of memory"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        set_error("could not start");
        client_stop();
    }
}

/* ---- publishing ---------------------------------------------------------- */

static bool pub(const char *topic, const char *body, bool retain)
{
    if (!s_client || !s_connected) return false;
    int r = esp_mqtt_client_publish(s_client, topic, body, (int)strlen(body), 0, retain);
    if (r < 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.published++;
    xSemaphoreGive(s_lock);
    return true;
}

static void dev_info(mqtt_dev_t *d, char *url, size_t ucap)
{
    const char *ip = netmgr_ip();
    if (ip && ip[0]) snprintf(url, ucap, "http://%s/", ip);
    else url[0] = '\0';
    d->id = s_id;
    d->name = netmgr_hostname();
    d->model = BOARD_NAME;
    d->sw = esp_app_get_description()->version;
    d->url = url;
    d->base = s_base;
}

static void publish_discovery(bool discovery_on)
{
    mqtt_dev_t d;
    char url[64];
    dev_info(&d, url, sizeof(url));
    snprintf(s_ctx->topic, sizeof(s_ctx->topic), "%s/status", s_base);
    pub(s_ctx->topic, "online", true);
    if (!discovery_on) return;
    for (int k = 0; k < mqtt_ups_entity_count(); k++) {
        if (mqtt_ups_disc(&d, k, s_ctx->topic, sizeof(s_ctx->topic), s_ctx->body, sizeof(s_ctx->body)) > 0) {
            pub(s_ctx->topic, s_ctx->body, true);
        }
    }
    out_cfg_t oc;
    outputs_get_cfg(&oc);
    if (!outputs_available()) return;
    for (int i = 0; i < OUT_N; i++) {
        for (int comp = 0; comp < 3; comp++) {
            int n = mqtt_out_disc(&d, i, &oc.pin[i], oc.mqtt_control, comp, s_ctx->topic,
                                  sizeof(s_ctx->topic), s_ctx->body, sizeof(s_ctx->body));
            if (n >= 0) pub(s_ctx->topic, s_ctx->body, true);
        }
    }
}

static void publish_outputs(void)
{
    if (!outputs_available()) return;
    out_cfg_t oc;
    outputs_get_cfg(&oc);
    for (int i = 0; i < OUT_N; i++) {
        snprintf(s_ctx->topic, sizeof(s_ctx->topic), "%s/output/%d", s_base, i + 1);
        /* An Off output's state is cleared, so nothing stale stays retained. */
        pub(s_ctx->topic, oc.pin[i].mode == OUT_MODE_OFF ? "" : outputs_is_on(i) ? "ON" : "OFF", true);
    }
}

static bool has_token(const char *status, const char *tok)
{
    size_t tl = strlen(tok);
    for (const char *p = status; *p; ) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == tl && !memcmp(p, tok, tl)) return true;
        p = e;
    }
    return false;
}

static float f(float v) { return ups_valid(v) ? v : MQTT_ABSENT; }

/* Build the readings; true if they differ from the last ones sent in a way
 * that should not wait (status, on battery, low battery). */
static void ups_build(mqtt_ups_t *u)
{
    static ups_data_t d;
    memset(u, 0, sizeof(*u));
    ups_hid_get(&d);
    char alarm[64];
    const char *reason = NULL;
    u->have_status = d.attached &&
                     ups_hid_nut_status(u->status, sizeof(u->status), alarm, sizeof(alarm), &reason);
    if (u->have_status) {
        u->on_battery = has_token(u->status, "OB");
        u->low_battery = has_token(u->status, "LB");
    }
    bool live = d.attached;
    u->charge = live ? f(d.battery_charge) : MQTT_ABSENT;
    u->runtime = live ? f(d.battery_runtime) : MQTT_ABSENT;
    u->load = live ? f(d.ups_load) : MQTT_ABSENT;
    u->in_v = live ? f(d.input_voltage) : MQTT_ABSENT;
    u->out_v = live ? f(d.output_voltage) : MQTT_ABSENT;
    u->batt_v = live ? f(d.battery_voltage) : MQTT_ABSENT;
    u->power = live ? f(ups_power_watts(&d, &u->power_est)) : MQTT_ABSENT;
    strlcpy(u->model, d.model, sizeof(u->model));
}

static void mqtt_task(void *arg)
{
    int64_t last_ups = 0;
    char last_key[64] = "";
    static mqtt_ups_t u;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        uint32_t fl = __atomic_exchange_n(&s_flags, 0, __ATOMIC_SEQ_CST);
        if (fl & F_RESTART) {
            client_stop();
            client_start();
            continue;
        }
        /* The owner changed whether MQTT may operate outputs: subscribe again. */
        out_cfg_t oc;
        outputs_get_cfg(&oc);
        if (s_client && oc.mqtt_control != s_control) {
            client_stop();
            client_start();
            continue;
        }
        if (!s_connected) continue;
        mqtt_cfg_t c;
        mqttlink_get_cfg(&c);
        bool disc = c.discovery;
        memset(&c, 0, sizeof(c));
        if (fl & F_DISCOVERY) publish_discovery(disc);
        if (fl & (F_OUTPUTS | F_DISCOVERY)) publish_outputs();
        ups_build(&u);
        char key[64];
        snprintf(key, sizeof(key), "%d|%s|%d%d", u.have_status, u.status, u.on_battery, u.low_battery);
        int64_t now = esp_timer_get_time();
        if ((fl & F_UPS) || strcmp(key, last_key) || now - last_ups >= UPS_EVERY_US) {
            if (mqtt_ups_json(&u, s_ctx->body, sizeof(s_ctx->body)) > 0) {
                snprintf(s_ctx->topic, sizeof(s_ctx->topic), "%s/ups", s_base);
                if (pub(s_ctx->topic, s_ctx->body, true)) {
                    strlcpy(last_key, key, sizeof(last_key));
                    last_ups = now;
                }
            }
        }
    }
}

static esp_err_t ensure_task(void)
{
    if (s_task) return ESP_OK;
    s_ctx = calloc(1, sizeof(*s_ctx));
    if (!s_ctx) return ESP_ERR_NO_MEM;
    if (xTaskCreate(mqtt_task, "mqtt_pub", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        free(s_ctx);
        s_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    outputs_set_listener(outputs_changed);
    return ESP_OK;
}

esp_err_t mqttlink_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, BOARD_ID_MAC_TYPE);
    snprintf(s_id, sizeof(s_id), "myupspal_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "myupspal/%s", netmgr_hostname());
    for (char *p = s_base; *p; p++) *p = (char)tolower((unsigned char)*p);
    load(&s_cfg);
    if (!s_cfg.enabled) return ESP_OK;
    esp_err_t err = ensure_task();
    if (err == ESP_OK) flag(F_RESTART);
    return err;
}

esp_err_t mqttlink_set_cfg(const mqtt_cfg_t *in, bool clear_pass, char *why, size_t why_len)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    mqtt_cfg_t c = *in;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* An empty password box keeps the saved one, unless the user name is
     * empty too: that is a broker with no login, and the page has no other
     * way to say so. */
    if (!c.pass[0] && !clear_pass && c.user[0]) strlcpy(c.pass, s_cfg.pass, sizeof(c.pass));
    xSemaphoreGive(s_lock);
    if (!mqtt_cfg_check(&c, why, why_len)) { memset(&c, 0, sizeof(c)); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = save(&c);
    if (err != ESP_OK) {
        if (why) snprintf(why, why_len, "could not save the settings");
        memset(&c, 0, sizeof(c));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = c;
    s_status.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    memset(&c, 0, sizeof(c));
    if (s_cfg.enabled) {
        err = ensure_task();
        if (err != ESP_OK) {
            if (why) snprintf(why, why_len, "not enough memory to start MQTT");
            return err;
        }
    }
    if (s_task) flag(F_RESTART);   /* off: the task stops the client and stays idle */
    return ESP_OK;
}
