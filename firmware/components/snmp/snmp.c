/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNMP on the board: settings in NVS, one task that answers UDP 161 and sends
 * traps. The protocol and the MIB are in snmp_core.c.
 *
 * One task does both, once a second: rebuild the view from the UPS snapshot,
 * send any traps that are owed, and in between answer requests. It is only
 * created once the owner turns SNMP on, so a board that never uses it pays
 * nothing for its stack.
 */
#include "snmp.h"
#include "snmp_core.h"
#include "ups_hid.h"
#include "nut_server.h"
#include "netmgr.h"
#include "board.h"
#include "nvs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "snmp";

#define NVS_NS        "upsa"      /* netmgr_factory_reset() erases it: the community goes too */
#define SNMP_PORT     161
#define RX_MAX        768         /* a request bigger than this is not one we answer */
#define TX_MAX        1400        /* fits one Ethernet frame with its headers */
#define TASK_STACK    4096
/* Answers per second, with a burst: SNMP over UDP is a favourite amplifier,
 * and a GETBULK answer is many times its request. The community is the real
 * barrier; this caps what a guessed one is worth. */
#define RATE_PER_S    20
#define RATE_BURST    40
/* A board starting up has no UPS for a few seconds; that is not "lost". */
#define COMMS_GRACE_US (90LL * 1000 * 1000)

static SemaphoreHandle_t s_lock;          /* s_cfg, s_gen, s_status */
static snmp_cfg_t        s_cfg;
static uint32_t          s_gen;           /* bumped by every saved change */
static snmp_status_t     s_status;
static TaskHandle_t      s_task;
/* Traps were on when the board started: only then is a coldStart true. */
static bool              s_traps_at_boot;

static SemaphoreHandle_t s_trap_lock;     /* s_trap_sock, s_trap_id, ctx->trap, ctx->test */
static int               s_trap_sock = -1;
static uint32_t          s_trap_id;

/* Everything big, in one allocation made when SNMP is first turned on: a
 * board that never uses it (most Rev As) keeps the RAM. */
typedef struct {
    uint8_t     rx[RX_MAX];
    uint8_t     tx[TX_MAX];
    uint8_t     trap[512];
    snmp_view_t view;             /* the task's */
    snmp_view_t test;             /* snmp_send_test()'s, under s_trap_lock */
    snmp_ev_t   ev;
    ups_data_t  ups;
} snmp_ctx_t;
static snmp_ctx_t *s_ctx;

/* ---- settings ----------------------------------------------------------- */

static void defaults(snmp_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->trap_port = SNMP_TRAP_PORT_DEFAULT;
}

static void nvs_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
}

static void load(snmp_cfg_t *c)
{
    defaults(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "snmp_agent", &u8) == ESP_OK) c->agent = u8 != 0;
    if (nvs_get_u8(h, "snmp_traps", &u8) == ESP_OK) c->traps = u8 != 0;
    if (nvs_get_u16(h, "snmp_port", &u16) == ESP_OK && u16) c->trap_port = u16;
    nvs_str(h, "snmp_comm", c->community, sizeof(c->community));
    nvs_str(h, "snmp_host", c->trap_host, sizeof(c->trap_host));
    nvs_str(h, "snmp_loc", c->location, sizeof(c->location));
    nvs_str(h, "snmp_con", c->contact, sizeof(c->contact));
    nvs_close(h);
    /* Whatever is in flash, it is never used unless it would be saved now. */
    if (!snmp_cfg_check(c, NULL, 0)) {
        ESP_LOGW(TAG, "saved settings do not check out; SNMP stays off");
        c->agent = c->traps = false;
    }
}

static esp_err_t save(const snmp_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(h, "snmp_agent", c->agent);
    if (err == ESP_OK) err = nvs_set_u8(h, "snmp_traps", c->traps);
    if (err == ESP_OK) err = nvs_set_u16(h, "snmp_port", c->trap_port);
    if (err == ESP_OK) err = nvs_set_str(h, "snmp_comm", c->community);
    if (err == ESP_OK) err = nvs_set_str(h, "snmp_host", c->trap_host);
    if (err == ESP_OK) err = nvs_set_str(h, "snmp_loc", c->location);
    if (err == ESP_OK) err = nvs_set_str(h, "snmp_con", c->contact);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void snmp_get_cfg(snmp_cfg_t *out)
{
    if (!s_lock) { defaults(out); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void snmp_get_status(snmp_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

static void snmp_task(void *arg);

static void ensure_task(void)
{
    if (s_task) return;
    if (!s_ctx) s_ctx = calloc(1, sizeof(*s_ctx));
    if (!s_ctx) {
        ESP_LOGE(TAG, "no memory for SNMP (%u bytes)", (unsigned)sizeof(*s_ctx));
        return;
    }
    /* Internal RAM: it is small, and lwIP calls run on it. */
    if (xTaskCreate(snmp_task, "snmp", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "could not start the SNMP task");
    }
}

esp_err_t snmp_set_cfg(const snmp_cfg_t *c, char *why, size_t why_len)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!snmp_cfg_check(c, why, why_len)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save(c);
    if (err != ESP_OK) {
        snprintf(why, why_len, "could not save: %s", esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *c;
    s_gen++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "agent %s, traps %s%s", c->agent ? "on" : "off", c->traps ? "to " : "off",
             c->traps ? c->trap_host : "");
    if (c->agent || c->traps) ensure_task();
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

/* ---- the view ----------------------------------------------------------- */

static float var_float(const char *name)
{
    char buf[24];
    if (!ups_hid_var_find(name, buf, sizeof(buf))) return UPS_VALUE_ABSENT;
    char *end;
    float f = strtof(buf, &end);
    /* strtof takes "nan" and "inf"; neither is a reading. */
    return end == buf || !(f > -1e9f && f < 1e9f) ? UPS_VALUE_ABSENT : f;
}

static uint32_t uptime_ticks(void)
{
    return (uint32_t)(esp_timer_get_time() / 10000);
}

static void identity(snmp_view_t *v, const snmp_cfg_t *c)
{
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(v->sys_descr, sizeof(v->sys_descr), "%s, firmware %s", BOARD_NAME, app->version);
    snprintf(v->sys_name, sizeof(v->sys_name), "%s", netmgr_hostname());
    snprintf(v->agent_ver, sizeof(v->agent_ver), "%s", app->version);
    snprintf(v->sys_location, sizeof(v->sys_location), "%s", c->location);
    snprintf(v->sys_contact, sizeof(v->sys_contact), "%s", c->contact);
    v->uptime = uptime_ticks();
}

static snmp_track_t s_track;
static bool         s_seen_ups;

static void build_view(snmp_view_t *v, const snmp_cfg_t *c)
{
    ups_data_t *d = &s_ctx->ups;
    char status[64], fw[48] = "";
    ups_hid_get(d);
    identity(v, c);
    bool live = nut_server_ups_status(d, status, sizeof(status));
    if (live) s_seen_ups = true;
    ups_hid_var_find("ups.firmware", fw, sizeof(fw));
    snmp_extra_t x = {
        .nut_status  = status,
        .stale       = !live,
        .comms_grace = !s_seen_ups && esp_timer_get_time() < COMMS_GRACE_US,
        .batt_temp   = var_float("battery.temperature"),
        .out_freq    = var_float("output.frequency"),
        .out_current = var_float("output.current"),
        .in_nominal  = var_float("input.voltage.nominal"),
        .out_nominal = var_float("output.voltage.nominal"),
        .ups_fw      = fw,
        .in_current       = var_float("input.current"),
        .in_freq_nominal  = var_float("input.frequency.nominal"),
        .out_freq_nominal = var_float("output.frequency.nominal"),
        .va_nominal       = var_float("ups.power.nominal"),
        .low_batt_s       = var_float("battery.runtime.low"),
        .xfer_low         = var_float("input.transfer.low"),
        .xfer_high        = var_float("input.transfer.high"),
    };
    x.nst_valid = live && ups_hid_nut_bits(&x.nst_bits);
    snmp_view_fill(&s_track, v, d, &x, esp_timer_get_time());
}

/* ---- traps -------------------------------------------------------------- */

static void count_trap(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) s_status.traps_sent++;
    else s_status.trap_errors++;
    xSemaphoreGive(s_lock);
}

/* Under s_trap_lock. */
static bool send_trap(const snmp_cfg_t *c, const snmp_view_t *v, const snmp_event_t *e)
{
    uint8_t *buf = s_ctx->trap;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(c->trap_port) };
    if (!snmp_ipv4_ok(c->trap_host, &to.sin_addr.s_addr)) return false;
    size_t n = snmp_build_trap(buf, sizeof(s_ctx->trap), c->community, ++s_trap_id, v, e);
    if (!n) return false;
    if (s_trap_sock < 0) {
        s_trap_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_trap_sock < 0) {
            ESP_LOGW(TAG, "trap socket: errno %d", errno);
            return false;
        }
    }
    bool ok = sendto(s_trap_sock, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)n;
    if (!ok) ESP_LOGW(TAG, "trap to %s:%u: errno %d", c->trap_host, c->trap_port, errno);
    count_trap(ok);
    return ok;
}

esp_err_t snmp_send_test(char *why, size_t why_len)
{
    snmp_cfg_t c;
    snmp_get_cfg(&c);
    if (!c.traps || !snmp_ipv4_ok(c.trap_host, NULL) || !c.community[0]) {
        snprintf(why, why_len, "turn traps on and save a receiver first");
        return ESP_ERR_INVALID_STATE;
    }
    if (netmgr_state() != NETMGR_STA_CONNECTED) {
        snprintf(why, why_len, "the board is not on its network");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ctx) {
        snprintf(why, why_len, "SNMP is not running (out of memory?)");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_trap_lock, portMAX_DELAY);
    snmp_view_t *v = &s_ctx->test;
    memset(v, 0, sizeof(*v));
    identity(v, &c);
    /* A warmStart: no IANA enterprise number of our own for a test
     * notification yet. The page says what the receiver will show. */
    snmp_event_t e = { SNMP_TRAP_WARM_START, 0 };
    bool ok = send_trap(&c, v, &e);
    xSemaphoreGive(s_trap_lock);
    if (!ok) snprintf(why, why_len, "could not send it; see the board's log");
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---- the task ----------------------------------------------------------- */

static int open_agent(void)
{
    /* Dual-stack only with IPv6 turned on; IPv4 clients then arrive
     * v4-mapped, and a reply sent to that form goes out over IPv4. */
    bool v6 = netmgr_ip6_enabled();
    int s = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGW(TAG, "socket: errno %d", errno);
        return -1;
    }
    struct sockaddr_storage a = { 0 };
    socklen_t al;
    if (v6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        int no = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons(SNMP_PORT);
        al = sizeof(*a6);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons(SNMP_PORT);
        al = sizeof(*a4);
    }
    if (bind(s, (struct sockaddr *)&a, al) != 0) {
        ESP_LOGW(TAG, "bind %d: errno %d", SNMP_PORT, errno);
        close(s);
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "agent listening on UDP %d", SNMP_PORT);
    return s;
}

static void snmp_task(void *arg)
{
    uint8_t *rx = s_ctx->rx, *tx = s_ctx->tx;
    snmp_view_t *view = &s_ctx->view;
    snmp_ev_t *ev = &s_ctx->ev;
    snmp_cfg_t c, was = { 0 };
    uint32_t gen = UINT32_MAX;
    /* coldStart says the board has just started: only to a receiver that was
     * set up when it did, never to one configured later. The task is made
     * when SNMP is first turned on, which may be long after start-up, so its
     * own first pass is not the test. */
    bool first = true;
    int sock = -1;
    int64_t next_tick = 0, bucket_at = 0;
    int bucket = RATE_BURST;
    int64_t last_refused_log = 0;
    /* A socket that will not open (lwIP out of sockets, say) is retried
     * with backoff, not every second, so it does not flood the log. */
    int64_t open_at = 0, open_backoff = 1000 * 1000;
    bool stack_logged = false;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool changed = gen != s_gen;
        c = s_cfg;
        gen = s_gen;
        xSemaphoreGive(s_lock);
        if (changed) {
            /* A new receiver, or traps turned on: it is told the alarms that
             * stand now, rather than nothing until the next change. A change
             * to anything else (the agent, the location) tells it nothing. */
            bool trap_change = first || c.traps != was.traps || c.trap_port != was.trap_port ||
                               strcmp(c.trap_host, was.trap_host) != 0 ||
                               strcmp(c.community, was.community) != 0;
            if (trap_change) snmp_ev_reset(ev, first && s_traps_at_boot && c.traps);
            first = false;
            was = c;
        }

        /* Not on the setup network (open to anyone in range), nor before
         * the board has an address. */
        bool net = netmgr_state() == NETMGR_STA_CONNECTED;
        if (sock < 0 && c.agent && net && esp_timer_get_time() >= open_at) {
            sock = open_agent();
            if (sock < 0) {
                open_at = esp_timer_get_time() + open_backoff;
                if (open_backoff < 60LL * 1000 * 1000) open_backoff *= 2;
            } else {
                open_backoff = 1000 * 1000;
            }
        }
        if (sock >= 0 && (!c.agent || !net)) {
            close(sock);
            sock = -1;
            ESP_LOGI(TAG, "agent closed");
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.listening = sock >= 0;
        xSemaphoreGive(s_lock);

        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick = now + 1000 * 1000;
            build_view(view, &c);
            if (!stack_logged && now > 5LL * 60 * 1000 * 1000) {
                /* Rev A is tight on RAM: this says whether 4 KB is right. */
                stack_logged = true;
                ESP_LOGI(TAG, "stack headroom %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            if (c.traps && net) {
                snmp_event_t evs[8];
                int n = snmp_events(ev, view, now, evs, 8);
                xSemaphoreTake(s_trap_lock, portMAX_DELAY);
                for (int i = 0; i < n; i++) send_trap(&c, view, &evs[i]);
                xSemaphoreGive(s_trap_lock);
            }
        }

        if (sock < 0) {
            /* Woken early by a settings change. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        int got = recvfrom(sock, rx, RX_MAX, 0, (struct sockaddr *)&from, &fl);
        if (got <= 0) continue;
        /* Over IPv6, only this network (netmgr_ip6.h); dropped unanswered,
         * before it spends from the rate bucket. */
        if (!netmgr_sockaddr_ok(&from, fl)) continue;

        /* Refill the bucket, then spend from it. */
        now = esp_timer_get_time();
        int64_t add = (now - bucket_at) * RATE_PER_S / 1000000;
        if (add > 0) {
            /* Keep the part of a token not yet earned, or a steady stream
             * just under the rate would never refill at the full rate. */
            if (bucket + add >= RATE_BURST) {
                bucket = RATE_BURST;
                bucket_at = now;
            } else {
                bucket += (int)add;
                bucket_at += add * 1000000 / RATE_PER_S;
            }
        }
        if (bucket <= 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.rate_limited++;
            xSemaphoreGive(s_lock);
            continue;
        }

        snmp_stats_t st;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        st = s_status.agent;
        xSemaphoreGive(s_lock);
        uint32_t bad_before = st.bad_community;
        view->uptime = uptime_ticks();
        size_t n = snmp_handle(rx, (size_t)got, tx, TX_MAX, c.community, view, &st);
        if (n) {
            bucket--;
            sendto(sock, tx, n, 0, (struct sockaddr *)&from, fl);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.agent = st;
        xSemaphoreGive(s_lock);
        if (st.bad_community != bad_before && now - last_refused_log > 60LL * 1000 * 1000) {
            last_refused_log = now;
            char ip[48];
            netmgr_sockaddr_str(&from, fl, ip, sizeof(ip));
            ESP_LOGW(TAG, "request from %s with the wrong community (logged once a minute)", ip);
        }
    }
}

esp_err_t snmp_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_trap_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_trap_lock) return ESP_ERR_NO_MEM;
    load(&s_cfg);
    s_traps_at_boot = s_cfg.traps;
    if (s_cfg.agent || s_cfg.traps) {
        ESP_LOGI(TAG, "agent %s, traps %s", s_cfg.agent ? "on" : "off", s_cfg.traps ? "on" : "off");
        ensure_task();
    }
    return ESP_OK;
}
