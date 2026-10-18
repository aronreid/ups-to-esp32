/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "webui.h"
#include "ups_hid.h"
#include "netmgr.h"
#include "lwip/sockets.h"
#include "metrics.h"
#include "fleet.h"
#include "crashlog.h"
#include "esp_mac.h"
#ifndef CONFIG_UPSA_NET_ETHERNET
#include "esp_wifi.h"
#endif
#include "nut_server.h"
#include "ota.h"
#include "auth.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "board.h"
#include "tailnet.h"
#include "outagelog.h"
#include "history.h"
#include "tlscert.h"
#if CONFIG_UPSA_TLS
#include "esp_https_server.h"
#endif
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "webui";

#if CONFIG_UPSA_TLS
/* The HTTPS server, when it is up; NULL before the certificate is ready, in
 * setup mode, or if it would not start. Port 80 is never turned away while
 * this is NULL, whatever s_require says, so a board whose HTTPS has failed
 * can still be reached. */
static httpd_handle_t s_https;
/* Port 80 answers with a redirect to 443 (NVS "tls"/"require"). */
static bool s_require;
#endif

/* Defined further down; used by handlers that come first. */
static bool may_read(httpd_req_t *req);
static esp_err_t send_err(httpd_req_t *req, const char *why);

/* The page, gzipped at build time (CMakeLists.txt). */
extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_gz_end");

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    /* Never cached: a firmware update changes this page, and a browser that
     * kept the old one showed old controls on new firmware. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* Append "name":value, or "name":null where the UPS does not report it.
 * null rather than 0 matters: the UI must show "--", and Home Assistant must
 * not graph a fabricated zero. */
/* Escape a string for a JSON string literal.
 *
 * Every string in this response comes from somewhere that can contain a quote:
 * a UPS's own model or manufacturer field, a Wi-Fi SSID someone chose, and --
 * the one that actually broke it -- an OTA error naming the two image
 * identities it refused to swap. An unescaped quote makes the whole document
 * invalid, so the page renders NOTHING rather than one bad field, which is a
 * far worse failure than the message it was trying to report.
 *
 * Writes into caller-provided storage and returns it, so it composes inside a
 * snprintf argument list. Truncates rather than overflowing, always
 * terminates, and never splits an escape across the end. */
static const char *jesc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (cap == 0) return "";
    if (!in) in = "";
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *two = NULL;
        switch (c) {
        case '"':  two = "\\\""; break;
        case '\\': two = "\\\\"; break;
        case '\n': two = "\\n";  break;
        case '\r': two = "\\r";  break;
        case '\t': two = "\\t";  break;
        default: break;
        }
        if (two) {
            if (o + 2 >= cap) break;
            out[o++] = two[0]; out[o++] = two[1];
        } else if (c < 0x20) {
            if (o + 6 >= cap) break;
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

static int json_num(char *p, size_t cap, const char *name, float v, bool last)
{
    if (ups_valid(v)) {
        return snprintf(p, cap, "\"%s\":%.2f%s", name, v, last ? "" : ",");
    }
    return snprintf(p, cap, "\"%s\":null%s", name, last ? "" : ",");
}

/* HOW MANY TIMES HAS THIS BOARD CRASHED? Counted in NVS, because uptime
 * and last_reset only describe the CURRENT life: a board that panics,
 * reboots in under a second and rejoins Wi-Fi looks perfectly healthy
 * thirty seconds later, and the only trace is a reset reason nobody reads.
 * One did exactly that here and it was found by chance.
 *
 * Counted once per boot, on the first request served -- /api/status or
 * /metrics, whichever comes first -- and only for the reasons that mean
 * something went wrong, not for an ordinary power-on or a deliberate restart
 * after an update. */
static uint32_t crash_count(void)
{
    static bool counted;
    static uint32_t crashes;
    if (!counted) {
        counted = true;
        esp_reset_reason_t rr = esp_reset_reason();
        nvs_handle_t h;
        if (nvs_open("upsa", NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u32(h, "crashes", &crashes);
            if (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT ||
                rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT ||
                rr == ESP_RST_BROWNOUT) {
                crashes++;
                nvs_set_u32(h, "crashes", crashes);
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    return crashes;
}

static esp_err_t get_status(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    /* Evidence for the OTA confirm gate: the server is answering, not merely
     * listening. Counted here rather than at registration because the bugs
     * that shipped let the device listen and then die on the first request. */
    ota_note_request_served();

    ups_data_t d;
    ups_hid_get(&d);

    const esp_app_desc_t *app = esp_app_get_description();

    /* Static, not on the stack. esp_http_server runs one task that services
     * requests sequentially, so there is no second handler to race with this,
     * and 2 KB is a quarter of that task's stack. Sized with room to spare
     * because snprintf truncates silently and the result is invalid JSON --
     * the page then shows nothing at all rather than one missing field. */
#if CONFIG_UPSA_TAILSCALE || CONFIG_UPSA_OUTAGE_LOG || CONFIG_UPSA_TLS
    static EXT_RAM_BSS_ATTR char buf[3712];      /* + the tailscale, outagelog, history and tls objects */
#else
    static EXT_RAM_BSS_ATTR char buf[3072];
#endif
    /* Escape scratch, static for the same reason as buf. Two bytes per input
     * byte is the worst case for a quote-heavy string, plus a terminator. */
    static EXT_RAM_BSS_ATTR char e1[200], e2[200], e3[200], e4[200], e5[24];
    int n = 0;
    /* Each append writes only into what is left. Past the end, sizeof(buf)-n
     * went negative, wrapped to a huge size_t, and the next snprintf wrote
     * through static memory: a few long error strings at once (crash, update,
     * join, heartbeat) were enough. A reply that does not fit is now a 500,
     * and the page says it could not read the board. */
#define AT(n)   (buf + ((n) < (int)sizeof(buf) ? (n) : (int)sizeof(buf) - 1))
#define ROOM(n) ((n) < (int)sizeof(buf) ? sizeof(buf) - (size_t)(n) : 1)
    n += snprintf(AT(n), ROOM(n), "{");
    /* Why the device last restarted, how long it has been up, and the worst
     * the heap has ever been. A board that reboots unattended is otherwise
     * indistinguishable from one that has simply been quiet -- and the reset
     * reason separates a brownout from a panic from someone pulling the plug,
     * which is the difference between a power supply problem and a bug. */
    /* Indexed by esp_reset_reason_t. The first version of this table had the
     * order wrong from index 5 on, so a genuine BROWNOUT -- the one cause this
     * field exists to catch -- displayed as "SDIO". The static assert below
     * pins the end of the table to the enum so a new IDF cannot shift it
     * silently. */
    static const char *RST[] = {
        [ESP_RST_UNKNOWN]   = "unknown",       [ESP_RST_POWERON] = "power-on",
        [ESP_RST_EXT]       = "external",      [ESP_RST_SW]      = "software",
        [ESP_RST_PANIC]     = "panic",         [ESP_RST_INT_WDT] = "interrupt watchdog",
        [ESP_RST_TASK_WDT]  = "task watchdog", [ESP_RST_WDT]     = "watchdog",
        [ESP_RST_DEEPSLEEP] = "deep sleep",    [ESP_RST_BROWNOUT] = "BROWNOUT",
        [ESP_RST_SDIO]      = "SDIO",          [ESP_RST_USB]     = "USB",
        [ESP_RST_JTAG]      = "JTAG",
    };
    _Static_assert(sizeof(RST) / sizeof(RST[0]) == ESP_RST_JTAG + 1,
                   "reset-reason table must match esp_reset_reason_t");
    esp_reset_reason_t rr = esp_reset_reason();

    uint32_t crashes = crash_count();
    const char *why = (rr < sizeof(RST) / sizeof(RST[0])) ? RST[rr] : "unknown";

    n += snprintf(AT(n), ROOM(n),
                  "\"board\":\"%s\",\"image\":\"%s\",\"version\":\"%s\","
                  "\"uptime_s\":%lld,\"last_reset\":\"%s\","
                  "\"crashes\":%u,"
                  "\"heap_free\":%u,\"heap_low\":%u,",
                  jesc(BOARD_NAME, e1, sizeof(e1)),
                  jesc(BOARD_IMAGE_NAME, e2, sizeof(e2)),
                  jesc(app->version, e3, sizeof(e3)),
                  (long long)(esp_timer_get_time() / 1000000),
                  jesc(why, e4, sizeof(e4)),
                  (unsigned)crashes,
                  (unsigned)esp_get_free_heap_size(),
                  (unsigned)esp_get_minimum_free_heap_size());
    /* A board on a cable has no Wi-Fi to show; the page hides the section
     * when it sees this. Absent on Wi-Fi boards. */
#ifdef CONFIG_UPSA_NET_ETHERNET
#define WIRED_JSON "\"wired\":true,"
#else
#define WIRED_JSON ""
#endif
    n += snprintf(AT(n), ROOM(n),
                  "\"wifi\":{" WIRED_JSON "\"state\":%d,\"rescue\":%s,\"ip\":\"%s\",\"ap\":\"%s\","
                  "\"ssid\":\"%s\",\"rssi\":%d,",
                  (int)netmgr_state(), netmgr_rescue() ? "true" : "false",
                  jesc(netmgr_ip(), e1, sizeof(e1)),
                  jesc(netmgr_ap_ssid(), e2, sizeof(e2)),
                  jesc(netmgr_ssid(), e3, sizeof(e3)), netmgr_rssi());
    /* The setup join: its progress, the address it was given and why it
     * failed. The setup page shows the address before the AP goes away,
     * which is the only moment a person can be told it without a router. */
    n += snprintf(AT(n), ROOM(n),
                  "\"host\":\"%s\",\"join\":%d,\"join_ip\":\"%s\","
                  "\"join_err\":\"%s\"},",
                  jesc(netmgr_hostname(), e1, sizeof(e1)), (int)netmgr_join_state(),
                  jesc(netmgr_join_ip(), e2, sizeof(e2)),
                  jesc(netmgr_join_error(), e3, sizeof(e3)));
    n += snprintf(AT(n), ROOM(n),
                  "\"nut\":{\"port\":%u,\"name\":\"%s\",\"clients\":%d},"
                  "\"ups\":{\"attached\":%s,\"mfr\":\"%s\",\"model\":\"%s\","
                  "\"vid\":\"%04x\",\"pid\":\"%04x\",\"status\":%lu,\"guard\":%d,",
                  NUT_DEFAULT_PORT, jesc(nut_server_ups_name(), e1, sizeof(e1)),
                  nut_server_clients(),
                  d.attached ? "true" : "false", jesc(d.mfr, e2, sizeof(e2)),
                  jesc(d.model, e3, sizeof(e3)),
                  d.vid, d.pid, (unsigned long)d.status, ups_hid_guard_level());
    n += json_num(AT(n), ROOM(n), "battery_charge",  d.battery_charge,  false);
    n += json_num(AT(n), ROOM(n), "battery_runtime", d.battery_runtime, false);
    n += json_num(AT(n), ROOM(n), "input_voltage",   d.input_voltage,   false);
    n += json_num(AT(n), ROOM(n), "output_voltage",  d.output_voltage,  false);
    n += json_num(AT(n), ROOM(n), "load",            d.ups_load,        false);
    n += json_num(AT(n), ROOM(n), "battery_voltage", d.battery_voltage, false);
    n += json_num(AT(n), ROOM(n), "input_frequency", d.input_frequency, false);
    n += json_num(AT(n), ROOM(n), "realpower",       d.ups_realpower,   false);
    n += json_num(AT(n), ROOM(n), "realpower_nom",   d.ups_realpower_nom, false);
    n += json_num(AT(n), ROOM(n), "apparentpower",   d.ups_apparentpower, false);
    n += json_num(AT(n), ROOM(n), "lowbatt_limit",   d.lowbatt_limit,   false);
    /* caps drives which controls the page offers: the UPS's own descriptor
     * decides, not a model name. */
    if (d.battery_mfr_date[0]) {
        n += snprintf(AT(n), ROOM(n),
                      "\"battery_mfr_date\":\"%s\",",
                      jesc(d.battery_mfr_date, e1, sizeof(e1)));
    } else {
        n += snprintf(AT(n), ROOM(n), "\"battery_mfr_date\":null,");
    }
    /* runtime_age_s: how long the UPS's runtime figure has gone unchanged,
     * -1 if never reported. desc: the report descriptor's fingerprint. */
    n += snprintf(AT(n), ROOM(n),
                  "\"runtime_age_s\":%lld,\"desc_hash\":\"%s\",\"desc_len\":%u,",
                  (long long)(d.runtime_changed_us
                              ? (esp_timer_get_time() - d.runtime_changed_us) / 1000000 : -1),
                  jesc(d.desc_hash, e1, sizeof(e1)), (unsigned)d.desc_len);
    n += snprintf(AT(n), ROOM(n),
                  "\"caps\":%lu,\"beeper\":%u,\"test_result\":%u",
                  (unsigned long)d.caps, d.beeper, d.test_result);
    ota_status_t o;
    ota_get(&o);
    n += snprintf(AT(n), ROOM(n),
                  "},\"vbus_fault\":%s,"
                  "\"ota\":{\"state\":%d,\"latest\":\"%s\",\"progress\":%d,"
                  "\"pending\":%s,\"auto\":%s,\"checked\":%s,\"dev\":%s,"
                  "\"error\":\"%s\","
                  "\"refused\":\"%s\",\"channel\":\"%s\",\"target\":\"%s\","
                  "\"auto_install\":%s},",
                  board_vbus_fault() ? "true" : "false",
                  (int)o.state, jesc(o.latest, e1, sizeof(e1)), o.progress,
                  o.pending_verify ? "true" : "false",
                  o.auto_check ? "true" : "false",
                  o.checked_once ? "true" : "false",
                  o.dev_build ? "true" : "false",
                  jesc(o.error, e2, sizeof(e2)), jesc(o.refused, e3, sizeof(e3)),
                  jesc(ota_channel_name(o.channel), e5, sizeof(e5)),
                  jesc(o.target, e4, sizeof(e4)),
                  o.auto_install ? "true" : "false");
    /* Where the last crash happened: the panic's reason and a backtrace to
     * decode against the release's ELF. "" if none has been recorded. */
    n += snprintf(AT(n), ROOM(n), "\"last_crash\":\"%s\",",
                  jesc(crashlog_last(), e1, sizeof(e1)));
    /* The fleet heartbeat: whether it is built in, what the owner allows, and
     * how the last one went. last_ok is seconds ago, -1 if never this boot. */
    fleet_status_t f;
    fleet_get(&f);
    char desc_res[64];
    fleet_descriptor_result(desc_res, sizeof(desc_res));
    int64_t now_s = esp_timer_get_time() / 1000000;
    n += snprintf(AT(n), ROOM(n),
                  "\"fleet\":{\"configured\":%s,\"enabled\":%s,\"stats\":%s,\"stats_auto\":%s,"
                  "\"channel\":\"%s\",\"sent\":%u,\"last_ok\":%lld,\"error\":\"%s\","
                  "\"desc\":\"%s\"},"
                  "\"auth\":{\"enabled\":%s}",
                  f.configured ? "true" : "false", f.enabled ? "true" : "false",
                  f.stats ? "true" : "false", f.stats_auto ? "true" : "false",
                  jesc(f.channel, e2, sizeof(e2)),
                  (unsigned)f.sent,
                  (long long)(f.last_ok_s < 0 ? -1 : now_s - f.last_ok_s),
                  jesc(f.last_error, e1, sizeof(e1)),
                  jesc(desc_res, e3, sizeof(e3)),
                  auth_enabled() ? "true" : "false");
#if CONFIG_UPSA_TAILSCALE
    /* Present only in a Tailscale build, so the page can tell "not in this
     * firmware" from "off". key_set, never the key: this JSON is served to
     * anyone who can reach the board. */
    tailnet_status_t ts;
    tailnet_get(&ts);
    n += snprintf(AT(n), ROOM(n),
                  ",\"tailscale\":{\"state\":\"%s\",\"enabled\":%s,"
                  "\"key_set\":%s,\"ip\":\"%s\",\"peers\":%d}",
                  jesc(tailnet_state_name(ts.state), e2, sizeof(e2)), ts.enabled ? "true" : "false",
                  ts.key_set ? "true" : "false", jesc(ts.ip, e1, sizeof(e1)), ts.peers);
#endif
#if CONFIG_UPSA_OUTAGE_LOG
    /* A summary for the page; the outages themselves are /api/outages. */
    outagelog_status_t ol;
    outagelog_status(&ol);
    n += snprintf(AT(n), ROOM(n),
                  ",\"outagelog\":{\"ready\":%s,\"active\":%s,\"clock\":%s,"
                  "\"outages\":%u,\"records\":%u,\"capacity\":%u,\"write_errors\":%u}",
                  ol.ready ? "true" : "false", ol.active ? "true" : "false",
                  ol.clock ? "true" : "false", (unsigned)ol.outages, (unsigned)ol.records,
                  (unsigned)ol.capacity, (unsigned)ol.write_errors);
#endif
#if CONFIG_UPSA_HISTORY
    history_status_t hs;
    history_status(&hs);
    n += snprintf(AT(n), ROOM(n), ",\"history\":{\"ready\":%s,\"days\":%u,\"write_errors\":%u}",
                  hs.ready ? "true" : "false", (unsigned)hs.days, (unsigned)hs.write_errors);
#endif
#if CONFIG_UPSA_TLS
    /* "https": whether this request came in over it, so the page can offer
     * the switch. ca_sha256 is the CA's, to check a download against. */
    n += snprintf(AT(n), ROOM(n),
                  ",\"tls\":{\"on\":%s,\"https\":%s,\"require\":%s,\"port\":443,"
                  "\"ca_sha256\":\"%s\",\"until\":%u}",
                  s_https ? "true" : "false",
                  (s_https && req->handle == s_https) ? "true" : "false",
                  s_require ? "true" : "false", tlscert_ca_sha256(),
                  (unsigned)tlscert_not_after());
#endif
    n += snprintf(AT(n), ROOM(n), "}");

#undef AT
#undef ROOM
    if (n < 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "/api/status needs %d bytes, has %u", n, (unsigned)sizeof(buf));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status too long");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void metrics_chunk(void *ctx, const char *text)
{
    httpd_resp_sendstr_chunk((httpd_req_t *)ctx, text);
}

/* Prometheus scrape target. Streamed line by line through metrics_render(),
 * so its size never has to fit a buffer: the UPS side grows with what the UPS
 * reports. See metrics.h for the naming. */
static esp_err_t get_metrics(httpd_req_t *req)
{
    /* Not on the open setup/rescue AP: anyone in radio range could read it. */
    struct sockaddr_in here;
    socklen_t hlen = sizeof(here);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&here, &hlen) == 0 &&
        netmgr_local_is_ap(here.sin_addr.s_addr)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    static ups_data_t d;                /* static for the stack; one server task */
    ups_hid_get(&d);
    const esp_app_desc_t *app = esp_app_get_description();
    metrics_src_t m = {
        .ups = &d,
        .ups_name = nut_server_ups_name(),
        .board = BOARD_NAME,
        .version = app->version,
        .hostname = netmgr_hostname(),
        .uptime_s = (uint64_t)(esp_timer_get_time() / 1000000),
        .heap_free = esp_get_free_heap_size(),
        .heap_min_free = esp_get_minimum_free_heap_size(),
        .wifi_rssi = netmgr_rssi(),
        .nut_clients = nut_server_clients(),
        .crashes = crash_count(),
    };
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    metrics_render(&m, metrics_chunk, req);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t deny(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* A request a browser sent from some OTHER page is refused, always: without
 * this, any web page open on any computer on the LAN could post load.off to
 * the board in the background, and the Origin header is the one thing that
 * page cannot forge. curl and scripts send no Origin and are unaffected. */
static bool origin_ok(httpd_req_t *req)
{
    char origin[128] = "", host[96] = "";
    esp_err_t oe = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    bool has_origin = oe == ESP_OK || oe == ESP_ERR_HTTPD_RESULT_TRUNC;
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (auth_origin_ok(has_origin ? origin : NULL, host)) return true;
    ESP_LOGW(TAG, "refused %s sent from another page (%s)", req->uri, origin);
    deny(req, "403 Forbidden", "{\"ok\":false,\"error\":\"refused: sent from another web page\"}");
    return false;
}

/* The page's token from /api/auth/login, or a username and password as HTTP
 * Basic for scripts -- curl -u user:pass. On failure the answer has been sent.
 * No WWW-Authenticate header on the 401, so a browser shows the page's own
 * login box rather than its native prompt. */
static uint32_t req_src(httpd_req_t *req)
{
    return auth_src_of_fd(httpd_req_to_sockfd(req));
}

static bool logged_in(httpd_req_t *req, const char *why)
{
    char tok[AUTH_TOKEN_LEN + 2];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", tok, sizeof(tok)) == ESP_OK &&
        auth_token_ok(tok)) {
        return true;
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[AUTH_USER_MAX + AUTH_PASS_MAX + 4];
        size_t dl = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6,
                                  strlen(hdr + 6)) == 0) {
            dec[dl] = '\0';
            char *colon = strchr((char *)dec, ':');
            if (colon) {
                *colon = '\0';
                auth_result_t r = auth_check(req_src(req), (char *)dec, colon + 1);
                memset(dec, 0, sizeof(dec));
                if (r == AUTH_OK) return true;
                if (r == AUTH_LOCKED) {
                    deny(req, "429 Too Many Requests",
                         "{\"ok\":false,\"login\":true,\"error\":\"too many wrong passwords; wait a minute\"}");
                    return false;
                }
            }
        }
    }
    char out[120];
    snprintf(out, sizeof(out), "{\"ok\":false,\"login\":true,\"error\":\"%s\"}", why);
    deny(req, "401 Unauthorized", out);
    return false;
}

/* EVERY ROUTE THAT CHANGES SOMETHING STARTS HERE, and returns at once if this
 * says no -- it has already answered. tools/test/test_auth.py fails the build
 * if a POST handler does not.
 *
 * THE LOGIN IS REQUIRED. A board with none yet -- fresh, or updated from a
 * release where it was optional -- refuses every change until one is
 * created (/api/auth/set), except on its own setup network, where the Wi-Fi
 * form creates it. Creating it is open to the first host on the LAN that asks
 * (/api/auth/set takes no login while none exists), and that host then owns
 * the board. Left as it is on purpose; documented in the README. Until
 * someone claims it, nothing else can be changed or cut off. */
static bool may_write(httpd_req_t *req)
{
    if (!origin_ok(req)) return false;
    if (!auth_enabled()) {
        if (netmgr_state() == NETMGR_AP_PROVISIONING) return true;
        deny(req, "403 Forbidden",
             "{\"ok\":false,\"needs_login\":true,\"error\":\"create a login for this board first\"}");
        return false;
    }
    return logged_in(req, "log in to change settings");
}

/* Every route that SHOWS something, once a login exists: the whole page sits
 * behind it. Before one exists the page can be read, so it can say so and
 * offer to create one. NUT reads and /metrics stay open, as with upsd -- a
 * monitor needs no login to watch. */
static bool may_read(httpd_req_t *req)
{
    if (!auth_enabled()) return true;
    return logged_in(req, "log in");
}

/* Read the whole request body into body (NUL-terminated); its length, or -1
 * with an error already sent. httpd_req_recv returns what one recv() had, so a
 * body split across segments (likelier over TLS or Tailscale) needs a loop. */
/* The whole body must arrive within this long. */
#define READ_BODY_MAX_US   (4 * 1000 * 1000)

static int read_body(httpd_req_t *req, char *body, size_t cap)
{
    int len = req->content_len;
    if (len <= 0 || len >= (int)cap) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return -1;
    }
    int got = 0, stalls = 0;
    /* One task serves every request on this server, so a client that sends
     * its body a byte at a time must not hold it: each recv returning a byte
     * resets the socket timeout, so the stall count alone never ends it.
     * An overall limit does (a real browser sends this in one segment). */
    const int64_t t0 = esp_timer_get_time();
    while (got < len) {
        if (esp_timer_get_time() - t0 > READ_BODY_MAX_US) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "request too slow");
            return -1;
        }
        int r = httpd_req_recv(req, body + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 3) continue;
        if (r <= 0) {
            httpd_resp_send_err(req, r == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT
                                                                 : HTTPD_400_BAD_REQUEST,
                                "could not read the request");
            return -1;
        }
        got += r;
    }
    body[got] = '\0';
    return got;
}

/* Read a small JSON body. NULL (and a 400 already sent) if it is not one. */
static cJSON *read_json(httpd_req_t *req, char *body, size_t cap)
{
    if (read_body(req, body, cap) < 0) return NULL;
    cJSON *root = cJSON_Parse(body);
    if (!root) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
    return root;
}

static const char *jstr(const cJSON *root, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Log in: {user, pass} -> {ok, token}. The token goes in X-Auth-Token on
 * every change the page makes. Only the cross-site check applies here -- a
 * login cannot itself require a login. */
static esp_err_t post_auth_login(httpd_req_t *req)
{
    char origin[128] = "", host[96] = "";
    esp_err_t oe = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (!auth_origin_ok(oe == ESP_OK || oe == ESP_ERR_HTTPD_RESULT_TRUNC ? origin : NULL, host)) {
        return deny(req, "403 Forbidden",
                    "{\"ok\":false,\"error\":\"refused: sent from another web page\"}");
    }
    char body[192];
    cJSON *root = read_json(req, body, sizeof(body));
    /* ESP_FAIL makes the server close the connection. ESP_OK would make it
     * read and throw away the rest of a body it never got, which a client
     * sending a byte at a time can stretch to minutes. The error is sent. */
    if (!root) return ESP_FAIL;
    char tok[AUTH_TOKEN_LEN + 1] = "";
    auth_result_t r = auth_enabled() ? auth_login(req_src(req), jstr(root, "user"), jstr(root, "pass"), tok)
                                     : AUTH_BAD;
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    if (r == AUTH_LOCKED) {
        return deny(req, "429 Too Many Requests",
                    "{\"ok\":false,\"error\":\"too many wrong passwords; wait a minute\"}");
    }
    if (r != AUTH_OK) {
        return deny(req, "401 Unauthorized",
                    auth_enabled() ? "{\"ok\":false,\"error\":\"wrong username or password\"}"
                                   : "{\"ok\":false,\"error\":\"no login is set\"}");
    }
    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"token\":\"%s\"}", tok);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_auth_logout(httpd_req_t *req)
{
    char tok[AUTH_TOKEN_LEN + 2];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", tok, sizeof(tok)) == ESP_OK) {
        auth_logout(tok);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Set, change or turn off the optional login: {user, pass}, user "" to turn
 * it off. While it is off anyone may set it -- the board is open to them
 * anyway, and it is how the owner opts in. While it is on, only someone
 * logged in may change it. Setting it logs this browser straight in. */
static esp_err_t post_auth_set(httpd_req_t *req)
{
    /* Creating the first login needs no login; changing it does. */
    if (auth_enabled() ? !may_write(req) : !origin_ok(req)) return ESP_OK;
    char body[192];
    cJSON *root = read_json(req, body, sizeof(body));
    /* ESP_FAIL makes the server close the connection. ESP_OK would make it
     * read and throw away the rest of a body it never got, which a client
     * sending a byte at a time can stretch to minutes. The error is sent. */
    if (!root) return ESP_FAIL;
    const char *user = jstr(root, "user"), *pass = jstr(root, "pass");
    bool off = !user || !user[0];
    if (off) {
        /* It used to be optional; it is not any more. */
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        return send_err(req, "a login is required: choose a username and password");
    }
    esp_err_t err = auth_set_credentials(off ? NULL : user, pass);
    char tok[AUTH_TOKEN_LEN + 1] = "";
    if (err == ESP_OK && !off) auth_login(req_src(req), user, pass, tok);
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"username 1-32 and password "
                               "8-64 characters, no spaces or quotes\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"could not save\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"enabled\":%s,\"token\":\"%s\"}",
             off ? "false" : "true", tok);
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_ups_reset(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    esp_err_t err = ups_hid_recover();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"this board hardwires VBUS\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* The setup form. From the setup AP this starts a join and returns at once;
 * the page follows it through /api/status. From a network, it stores and
 * reboots -- and replies BEFORE that, which is why netmgr_provision defers the
 * restart: a page that says "saved" and then loses the socket is correct, a
 * page that reports a failure for something that worked is not. */
/* {"static":bool,"ip","mask","gw","dns"} -> a netmgr_ipcfg_t. False with a
 * reason when a fixed address is asked for and is not usable. */
static bool ipcfg_from_json(const cJSON *o, netmgr_ipcfg_t *c, char *why, size_t n)
{
    memset(c, 0, sizeof(*c));
    if (!cJSON_IsObject(o)) return true;                  /* absent: DHCP */
    c->is_static = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "static"));
    if (!c->is_static) return true;
    const struct { const char *k; char *dst; } f[] = {
        {"ip", c->ip}, {"mask", c->mask}, {"gw", c->gw}, {"dns", c->dns},
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, f[i].k);
        if (cJSON_IsString(v)) strlcpy(f[i].dst, v->valuestring, 16);
    }
    return netmgr_ipcfg_check(c, why, n);
}

/* The error body, escaped: why can quote what the person typed. */
static esp_err_t send_err(httpd_req_t *req, const char *why)
{
    static char e[200], out[260];
    snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", jesc(why, e, sizeof(e)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[448];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    netmgr_ipcfg_t ip;
    char why[80];
    if (!ipcfg_from_json(cJSON_GetObjectItemCaseSensitive(root, "ip"), &ip, why, sizeof(why))) {
        cJSON_Delete(root);
        return send_err(req, why);
    }

    /* The setup form's login: REQUIRED on a board that has none. Saved before
     * the join, and checked first so a bad one is reported rather than the
     * board leaving setup unprotected. Ignored if a login already exists:
     * changing it is /api/auth/set's job, behind that login. */
    const char *au = jstr(root, "admin_user"), *ap = jstr(root, "admin_pass");
    if (!auth_enabled() && !(au && au[0])) {
        cJSON_Delete(root);
        return send_err(req, "choose a username and password for this board");
    }
    /* Everything else is checked before the login is committed: it is saved
     * once, so a retry with a corrected form would be silently ignored. */
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(root);
        return send_err(req, "a network name is required");
    }
    bool made_login = false;
    if (au && au[0] && !auth_enabled()) {
        esp_err_t aerr = auth_set_credentials(au, ap);
        if (aerr != ESP_OK) {
            cJSON_Delete(root);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, aerr == ESP_ERR_INVALID_ARG
                ? "{\"ok\":false,\"error\":\"login: username 1-32 and password 8-64 "
                  "characters, no spaces or quotes\"}"
                : "{\"ok\":false,\"error\":\"could not save the login\"}",
                HTTPD_RESP_USE_STRLEN);
        }
        made_login = true;
    }

    bool setup = netmgr_state() == NETMGR_AP_PROVISIONING;
    esp_err_t err = netmgr_provision(ssid->valuestring,
                                     cJSON_IsString(pass) ? pass->valuestring : "", &ip);
    cJSON_Delete(root);
    if (err != ESP_OK && made_login) auth_set_credentials(NULL, NULL);   /* roll back */

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        return httpd_resp_send(req,
            err == ESP_ERR_INVALID_ARG
              ? "{\"ok\":false,\"error\":\"a network name is required\"}"
              : err == ESP_ERR_INVALID_STATE
              ? "{\"ok\":false,\"error\":\"already trying a network, one moment\"}"
              : "{\"ok\":false,\"error\":\"could not start joining\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    if (setup) {
        return httpd_resp_send(req, "{\"ok\":true,\"joining\":true}",
                               HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI(TAG, "provisioned, restarting");
    return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_scan(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    netmgr_ap_t aps[16];
    int n = netmgr_scan(aps, sizeof(aps) / sizeof(aps[0]));

    httpd_resp_set_type(req, "application/json");
    if (n < 0) {
        return httpd_resp_send(req, "{\"ok\":false,\"nets\":[]}",
                               HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"nets\":[");
    for (int i = 0; i < n; i++) {
        char tail[48];
        /* The SSID goes through cJSON's escaper: an apostrophe or a backslash
         * in a network name would otherwise produce JSON the page cannot
         * parse, and people do name networks like that. It is sent as a chunk
         * of its own, not squeezed into a fixed buffer: 32 bytes of quotes
         * escape to 192, and a truncated entry broke the whole array. */
        cJSON *str = cJSON_CreateString(aps[i].ssid);
        char *esc = str ? cJSON_PrintUnformatted(str) : NULL;
        httpd_resp_sendstr_chunk(req, i ? ",{\"ssid\":" : "{\"ssid\":");
        httpd_resp_sendstr_chunk(req, esc ? esc : "\"?\"");
        snprintf(tail, sizeof(tail), ",\"rssi\":%d,\"secure\":%s}", aps[i].rssi,
                 aps[i].secure ? "true" : "false");
        cJSON_free(esc);
        cJSON_Delete(str);
        httpd_resp_sendstr_chunk(req, tail);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Anything we do not serve, while provisioning, is a captive-portal probe.
 *
 * Phones fetch a known URL after joining and decide from the answer whether
 * the network is usable; a redirect makes them show the setup page by
 * themselves. On a normal network this same handler is an ordinary 404, since
 * redirecting every stray request would break a browser rather than help it.
 */
static esp_err_t get_catchall(httpd_req_t *req)
{
    if (netmgr_state() != NETMGR_AP_PROVISIONING) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such page");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* UPS commands. Everything here is refused by ups_hid_command() unless the
 * attached UPS's descriptor actually carries the usage, so a request for a
 * self-test on a UPS that has none is a 501 rather than a silent no-op. */
/* NUT's own instcmd names, so the HTTP API and the NUT server speak one
 * vocabulary and the dispatch on 3493 is a lookup rather than a translation.
 * battery.charge.low and battery.date are NUT read-write VARIABLES rather than
 * commands; they are accepted here too because this API has no SET VAR. */
static const struct { const char *name; ups_cmd_t cmd; bool takes_arg; } CMDS[] = {
    { "test.battery.start.quick", UPS_CMD_TEST_QUICK,     false },
    { "test.battery.start.deep",  UPS_CMD_TEST_DEEP,      false },
    { "test.battery.stop",        UPS_CMD_TEST_ABORT,     false },
    { "beeper.disable",           UPS_CMD_BEEPER_DISABLE, false },
    { "beeper.enable",            UPS_CMD_BEEPER_ENABLE,  false },
    { "beeper.mute",              UPS_CMD_BEEPER_MUTE,    false },
    { "battery.charge.low",       UPS_CMD_LOWBATT_LIMIT,  true  },
    { "load.off",                 UPS_CMD_LOAD_OFF,       true  },
    { "battery.date",             UPS_CMD_BATTERY_DATE,   true  },
};

static esp_err_t post_cmd(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[160];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
        return ESP_FAIL;
    }
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *ja = cJSON_GetObjectItemCaseSensitive(root, "arg");
    int32_t arg = cJSON_IsNumber(ja) ? (int32_t)ja->valuedouble : 0;

    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *name = cJSON_IsString(jc) ? jc->valuestring : "";
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (strcmp(name, CMDS[i].name) != 0) continue;
        ESP_LOGI(TAG, "command %s arg %ld", name, (long)arg);
        err = ups_hid_command(CMDS[i].cmd, arg);
        break;
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    const char *why = "the UPS refused it";
    if (err == ESP_ERR_NOT_SUPPORTED)      why = "this UPS does not support that";
    else if (err == ESP_ERR_INVALID_STATE) why = "no UPS is attached";
    else if (err == ESP_ERR_INVALID_ARG)   why = "unknown command";
    else if (err == ESP_ERR_TIMEOUT)       why = "the UPS did not answer";
    else if (err == ESP_ERR_NOT_ALLOWED)   why = "a deep test needs a fully charged battery (100 %)";

    char out[128];
    int n = snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_send(req, out, n);
}

/* The raw report descriptor, exactly as the UPS returned it.
 *
 * Served as a file rather than as JSON so that
 *     curl -O http://ups-esp32-XXXX.local/api/descriptor
 * produces a capture byte-identical to the ones under captures/, which
 * tools/decode-hid.py already reads. This firmware's parsing is generic by
 * design, and a descriptor it handles badly is the only useful bug report. */
static esp_err_t get_descriptor(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    const uint8_t *desc = NULL;
    size_t len = ups_hid_report_descriptor(&desc);
    if (!len) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no UPS attached");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"report-descriptor.bin\"");
    return httpd_resp_send(req, (const char *)desc, len);
}

/* What the board and an APC Smart-UPS said to each other over Modbus
 * (ups_hid_modbus_json), as a file a tester can send back:
 *     curl -o modbus.json http://ups-esp32-XXXX.local/api/modbus
 * No Smart-UPS with Modbus has been on the bench, so this is how its raw
 * registers get checked against NUT's reading of the same unit. */
#define MODBUS_JSON_MAX 8192
static esp_err_t get_modbus(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    char *buf = malloc(MODBUS_JSON_MAX);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    size_t n = ups_hid_modbus_json(buf, MODBUS_JSON_MAX);
    if (!n) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"modbus.json\"");
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return err;
}

/* The offered release's abridged notes, for the update card. A route of its
 * own rather than part of /api/status: it is up to a kilobyte, it changes only
 * when a check finds something new, and the page fetches it once per release
 * rather than every three seconds. Built with cJSON because the text is a
 * person's, from a tag message, and may hold anything. */
static esp_err_t get_ota_notes(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    static EXT_RAM_BSS_ATTR char tag[48], url[160], notes[1024];
    ota_get_notes(tag, sizeof(tag), url, sizeof(url), notes, sizeof(notes));

    cJSON *root = cJSON_CreateObject();
    cJSON *items = root ? cJSON_AddArrayToObject(root, "notes") : NULL;
    char *out = NULL;
    if (items) {
        cJSON_AddStringToObject(root, "tag", tag);
        cJSON_AddStringToObject(root, "url", url);
        for (char *line = notes, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = '\0';
            cJSON_AddItemToArray(items, cJSON_CreateString(line));
        }
        out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (!out) return httpd_resp_send(req, "{\"tag\":\"\",\"url\":\"\",\"notes\":[]}",
                                     HTTPD_RESP_USE_STRLEN);
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static esp_err_t post_ota_check(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    /* Check in too: if the project has moved this board to another tier or
     * named a release for it, the reply says so and triggers its own check. */
    fleet_checkin_soon();
    esp_err_t err = ota_check();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}"
                           : "{\"ok\":false,\"error\":\"busy\"}",
                           HTTPD_RESP_USE_STRLEN);
}

/* Installing writes the other slot and leaves the running image alone, so this
 * is safe to trigger and only takes effect on the reboot that follows. */
static esp_err_t post_ota_install(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    esp_err_t err = ota_install();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}"
                           : "{\"ok\":false,\"error\":\"busy\"}",
                           HTTPD_RESP_USE_STRLEN);
}

/* Turns the periodic CHECK on or off. There is deliberately no equivalent for
 * installing: that always needs a person, so there is nothing to configure. */
/* {"enabled":bool,"stats":bool}: the owner's fleet reporting choices. */
static esp_err_t post_fleet(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[96];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */

    cJSON *root = cJSON_Parse(body);
    const cJSON *en = root ? cJSON_GetObjectItemCaseSensitive(root, "enabled") : NULL;
    const cJSON *st = root ? cJSON_GetObjectItemCaseSensitive(root, "stats") : NULL;
    esp_err_t err = (cJSON_IsBool(en) && cJSON_IsBool(st))
                    ? fleet_set(cJSON_IsTrue(en), cJSON_IsTrue(st))
                    : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}"
                           : "{\"ok\":false,\"error\":\"bad request\"}",
                           HTTPD_RESP_USE_STRLEN);
}

/* "Send my UPS info": the owner's one-click report of a UPS that does not
 * work. Returns at once; /api/status carries fleet.desc, how it went. */
static esp_err_t post_fleet_descriptor(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    const uint8_t *d;
    if (!ups_hid_report_descriptor(&d)) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no UPS attached\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    fleet_send_descriptor();
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* The address: what is stored, what is in use, and whether a fixed address
 * failed its gateway check this session. */
static esp_err_t get_network(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    netmgr_ipcfg_t c, cur;
    bool fell_back;
    netmgr_ipcfg_get(&c, &fell_back);
    netmgr_ipcfg_current(&cur);
    static EXT_RAM_BSS_ATTR char out[512];
    static char a[40], b[40], d[40], e[40], f[40], g[40], h[40], i[40];
    snprintf(out, sizeof(out),
             "{\"static\":%s,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
             "\"fell_back\":%s,\"current\":{\"static\":%s,\"ip\":\"%s\",\"mask\":\"%s\","
             "\"gw\":\"%s\",\"dns\":\"%s\"}}",
             c.is_static ? "true" : "false", jesc(c.ip, a, sizeof(a)), jesc(c.mask, b, sizeof(b)),
             jesc(c.gw, d, sizeof(d)), jesc(c.dns, e, sizeof(e)), fell_back ? "true" : "false",
             cur.is_static ? "true" : "false", jesc(cur.ip, f, sizeof(f)),
             jesc(cur.mask, g, sizeof(g)), jesc(cur.gw, h, sizeof(h)), jesc(cur.dns, i, sizeof(i)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_network(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[256];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */
    cJSON *root = cJSON_Parse(body);
    netmgr_ipcfg_t c;
    char why[80] = "not JSON";
    bool ok = root && ipcfg_from_json(root, &c, why, sizeof(why)) &&
              netmgr_ipcfg_set(&c, why, sizeof(why)) == ESP_OK;
    cJSON_Delete(root);
    if (!ok) return send_err(req, why);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Everything about the board itself, for the page's Debug card: what a test
 * board sends the fleet (fleet_diag) plus what only makes sense locally. */
/* Every NUT variable NUT's own table found on this UPS (ups_vartab.h), with
 * its current value: what a NUT client sees beyond the page's own cards.
 * {"table":"apc","vars":[["battery.type","PbAc"],...]}. */
static esp_err_t get_vars(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "vars") : NULL;
    char *out = NULL;
    if (arr) {
        cJSON_AddStringToObject(root, "table", ups_hid_var_table());
        size_t n = ups_hid_var_count();
        for (size_t i = 0; i < n; i++) {
            const char *name;
            char v[40];
            if (!ups_hid_var_get(i, &name, v, sizeof(v))) continue;
            cJSON *pair = cJSON_CreateArray();
            if (!pair) break;
            cJSON_AddItemToArray(pair, cJSON_CreateString(name));
            cJSON_AddItemToArray(pair, cJSON_CreateString(v));
            cJSON_AddItemToArray(arr, pair);
        }
        out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!out) return httpd_resp_send(req, "{\"table\":\"\",\"vars\":[]}", HTTPD_RESP_USE_STRLEN);
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

static esp_err_t get_debug(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    cJSON *o = fleet_diag();
    if (!o) o = cJSON_CreateObject();
    if (!o) { httpd_resp_send_500(req); return ESP_FAIL; }
    uint8_t mac[6];
    char s[40];
    esp_read_mac(mac, BOARD_ID_MAC_TYPE);
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "mac", s);
    cJSON_AddNumberToObject(o, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(o, "tasks", uxTaskGetNumberOfTasks());
    /* The least stack this server's task has had left, in bytes: the one
     * answering, so over HTTPS it is the HTTPS server's. What its size was
     * set by. */
    cJSON_AddNumberToObject(o, "stack_left", uxTaskGetStackHighWaterMark(NULL));
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "reset_reason", esp_reset_reason());
    cJSON_AddStringToObject(o, "last_crash", crashlog_last());
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        snprintf(s, sizeof(s), "%u of %u entries", (unsigned)ns.used_entries,
                 (unsigned)ns.total_entries);
        cJSON_AddStringToObject(o, "nvs", s);
    }
    netmgr_ipcfg_t c;
    netmgr_ipcfg_current(&c);
    cJSON_AddStringToObject(o, "mask", c.mask);
    cJSON_AddStringToObject(o, "gw", c.gw);
    cJSON_AddStringToObject(o, "dns", c.dns);
    cJSON_AddBoolToObject(o, "ip_fixed", c.is_static);
    cJSON_AddStringToObject(o, "ssid", netmgr_ssid());
    cJSON_AddNumberToObject(o, "rssi", netmgr_rssi());
    ota_status_t ot;
    ota_get(&ot);
    cJSON_AddStringToObject(o, "tier", ota_channel_name(ot.channel));
    cJSON_AddStringToObject(o, "tier_target", ot.target);
    cJSON_AddBoolToObject(o, "auto_install", ot.auto_install);
    fleet_status_t f;
    fleet_get(&f);
    cJSON_AddNumberToObject(o, "checkins", f.sent);
    cJSON_AddNumberToObject(o, "last_checkin_s_ago",
                            f.last_ok_s < 0 ? -1 : (double)(esp_timer_get_time() / 1000000 - f.last_ok_s));
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!js) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
    free(js);
    return err;
}

static esp_err_t post_ota_auto(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[64];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */

    cJSON *root = cJSON_Parse(body);
    const cJSON *en = root ? cJSON_GetObjectItemCaseSensitive(root, "enabled") : NULL;
    esp_err_t err = cJSON_IsBool(en) ? ota_set_auto_check(cJSON_IsTrue(en))
                                     : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}"
                           : "{\"ok\":false,\"error\":\"bad request\"}",
                           HTTPD_RESP_USE_STRLEN);
}

#if CONFIG_UPSA_TAILSCALE
/* Tailscale settings: {"enabled":bool, "key":"tskey-...", "forget":bool}.
 * key is optional -- omitted or "" keeps the stored one, so the switch can be
 * flipped without pasting the key again, and the page never needs it back.
 * Behind the login (may_write) like every other change. Written before the
 * login existed, this took an auth key from anyone on the LAN -- enough to
 * join the board to someone else's tailnet. */
static esp_err_t post_tailscale(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[256];
    int got = read_body(req, body, sizeof(body));
    if (got < 0) return ESP_FAIL;               /* the error has been sent */

    cJSON *root = cJSON_Parse(body);
    /* The body held the key; it has been parsed and is not needed again. */
    memset(body, 0, sizeof(body));
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
        return ESP_FAIL;
    }
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    const cJSON *forget = cJSON_GetObjectItemCaseSensitive(root, "forget");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsBool(en)) {
        err = tailnet_configure(cJSON_IsTrue(en),
                                cJSON_IsString(key) ? key->valuestring : NULL,
                                cJSON_IsTrue(forget));
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req,
        err == ESP_ERR_INVALID_ARG
          ? "{\"ok\":false,\"error\":\"that does not look like a Tailscale auth key (tskey-...)\"}"
          : "{\"ok\":false,\"error\":\"could not save the setting\"}",
        HTTPD_RESP_USE_STRLEN);
}
#endif

#if CONFIG_UPSA_OUTAGE_LOG
static void chunk_emit(void *ctx, const char *text)
{
    httpd_resp_sendstr_chunk((httpd_req_t *)ctx, text);
}

/* The outage log, newest first. Streamed: up to a hundred outages. */
static esp_err_t get_outages(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    outagelog_json_list(chunk_emit, req, 100);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* One outage's readings: /api/outages/samples?id=N. */
static esp_err_t get_outage_samples(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    char q[32], v[12];
    uint32_t id = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) {
        id = (uint32_t)strtoul(v, NULL, 10);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!outagelog_json_samples(id, chunk_emit, req)) return send_err(req, "no such outage");
    return httpd_resp_sendstr_chunk(req, NULL);
}

#if CONFIG_UPSA_HISTORY
/* Long history for the chart: /api/history?span=SECONDS&points=N. */
static esp_err_t get_history(httpd_req_t *req)
{
    if (!may_read(req)) return ESP_OK;
    char q[48], v[12];
    uint32_t span = 3600, points = 300;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "span", v, sizeof(v)) == ESP_OK) span = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "points", v, sizeof(v)) == ESP_OK) points = (uint32_t)strtoul(v, NULL, 10);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    history_json(span, points, chunk_emit, req);
    return httpd_resp_sendstr_chunk(req, NULL);
}
#endif

/* Forget the outages recorded so far. Nothing is erased; see outagelog_clear(). */
static esp_err_t post_outages_clear(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    esp_err_t err = outagelog_clear();
    if (err != ESP_OK) return send_err(req, "could not clear the outage log");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t post_reboot(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(TAG, "reboot requested over HTTP");
    vTaskDelay(pdMS_TO_TICKS(400));      /* let the reply leave first */
    esp_restart();
    return ESP_OK;
}

/* Not served here: a factory reset. Clearing the network is what makes a board
 * in a closet unreachable, so the only way to ask for it is to be standing in
 * front of it, pressing BOOT five times -- see factory_gesture_poll in main.c. Firmware
 * update lives under /api/ota (check, install, auto) and pulls a published
 * release rather than accepting an uploaded image. */

/* One place that knows how many routes there are, used both to size the
 * server's handler table and to walk it. */
#if CONFIG_UPSA_TLS
/* The board's CA, to install so the browser stops warning. No login: it is a
 * public certificate, and a plain link from the page could not carry the
 * page's token. */
static esp_err_t get_tls_ca(httpd_req_t *req)
{
    const char *pem = tlscert_ca_pem();
    if (!pem) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no certificate yet");
        return ESP_FAIL;
    }
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s-ca.crt\"", netmgr_hostname());
    httpd_resp_set_type(req, "application/x-x509-ca-cert");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    return httpd_resp_send(req, pem, HTTPD_RESP_USE_STRLEN);
}

/* {"require":true}: port 80 redirects to 443. Refused while HTTPS is not up,
 * since that would leave nothing to redirect to. */
static esp_err_t post_tls(httpd_req_t *req)
{
    if (!may_write(req)) return ESP_OK;
    char body[64];
    cJSON *root = read_json(req, body, sizeof(body));
    if (!root) return ESP_FAIL;
    const cJSON *rq = cJSON_GetObjectItemCaseSensitive(root, "require");
    bool want = cJSON_IsTrue(rq);
    bool valid = cJSON_IsBool(rq);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!valid) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"expected {\\\"require\\\":true|false}\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    if (want && !s_https) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"HTTPS is not running on this board\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("tls", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "require", want ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"could not save the setting\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    s_require = want;
    ESP_LOGI(TAG, "port 80 %s", want ? "now redirects to HTTPS" : "serves the page again");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Port 80's answer to everything while HTTPS is required. 302 and 307, not
 * 301 and 308: a browser keeps a permanent redirect for good, and port 80
 * would stay unreachable from it after the setting is turned off. */
static esp_err_t to_https(httpd_req_t *req)
{
    char host[96] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *port = host[0] == '[' ? strstr(host, "]:") : strchr(host, ':');
    if (port) port[host[0] == '[' ? 1 : 0] = '\0';
    if (!host[0]) snprintf(host, sizeof(host), "%s", netmgr_ip());
    char loc[8 + sizeof(host) + CONFIG_HTTPD_MAX_URI_LEN];
    snprintf(loc, sizeof(loc), "https://%s%s", host, req->uri);
    httpd_resp_set_status(req, req->method == HTTP_GET || req->method == HTTP_HEAD
                               ? "302 Found" : "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}
#endif

/* Every route goes through here, with the real handler in user_ctx. Two
 * servers (80 and, on Rev B, 443) run the same handlers on two tasks, and
 * several handlers keep their buffers static on the understanding that one
 * server answers one request at a time: the lock keeps that true across
 * both. */
typedef esp_err_t (*handler_fn)(httpd_req_t *req);
static SemaphoreHandle_t s_req_lock;

static esp_err_t route(httpd_req_t *req)
{
    handler_fn h = (handler_fn)req->user_ctx;
#if CONFIG_UPSA_TLS
    if (s_require && s_https && req->handle != s_https &&
        netmgr_state() != NETMGR_AP_PROVISIONING) {
        return to_https(req);
    }
#endif
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    esp_err_t r = h(req);
    xSemaphoreGive(s_req_lock);
    return r;
}

/* The page's routes: how many there are sizes each server's handler table. */
#if CONFIG_UPSA_TAILSCALE
#define ROUTES_TAILSCALE 1
#else
#define ROUTES_TAILSCALE 0
#endif
#if CONFIG_UPSA_OUTAGE_LOG
#define ROUTES_OUTAGELOG 3
#else
#define ROUTES_OUTAGELOG 0
#endif
#if CONFIG_UPSA_HISTORY
#define ROUTES_HISTORY 1
#else
#define ROUTES_HISTORY 0
#endif
#if CONFIG_UPSA_TLS
#define ROUTES_TLS 2
#else
#define ROUTES_TLS 0
#endif
#define ROUTE_COUNT (24 + ROUTES_TAILSCALE + ROUTES_OUTAGELOG + ROUTES_HISTORY + ROUTES_TLS)

/* Order matters: the server tries these in turn and the wildcard route
 * matches everything, so it has to be last or it swallows the API. */
static const struct { const char *uri; httpd_method_t method; handler_fn fn; } ROUTES[] = {
    { "/",                     HTTP_GET,  get_root },
    { "/api/status",           HTTP_GET,  get_status },
    { "/metrics",              HTTP_GET,  get_metrics },
    { "/api/scan",             HTTP_GET,  get_scan },
    { "/api/descriptor",       HTTP_GET,  get_descriptor },
    { "/api/modbus",           HTTP_GET,  get_modbus },
    { "/api/wifi",             HTTP_POST, post_wifi },
    { "/api/ups/reset",        HTTP_POST, post_ups_reset },
    { "/api/cmd",              HTTP_POST, post_cmd },
    { "/api/ota/notes",        HTTP_GET,  get_ota_notes },
    { "/api/ota/check",        HTTP_POST, post_ota_check },
    { "/api/ota/install",      HTTP_POST, post_ota_install },
    { "/api/ota/auto",         HTTP_POST, post_ota_auto },
    { "/api/fleet",            HTTP_POST, post_fleet },
    { "/api/fleet/descriptor", HTTP_POST, post_fleet_descriptor },
    { "/api/network",          HTTP_GET,  get_network },
    { "/api/network",          HTTP_POST, post_network },
    { "/api/debug",            HTTP_GET,  get_debug },
    { "/api/vars",             HTTP_GET,  get_vars },
    { "/api/reboot",           HTTP_POST, post_reboot },
    { "/api/auth/login",       HTTP_POST, post_auth_login },
    { "/api/auth/logout",      HTTP_POST, post_auth_logout },
    { "/api/auth/set",         HTTP_POST, post_auth_set },
#if CONFIG_UPSA_TAILSCALE
    { "/api/tailscale",        HTTP_POST, post_tailscale },
#endif
#if CONFIG_UPSA_OUTAGE_LOG
    { "/api/outages",          HTTP_GET,  get_outages },
    { "/api/outages/samples",  HTTP_GET,  get_outage_samples },
    { "/api/outages/clear",    HTTP_POST, post_outages_clear },
#endif
#if CONFIG_UPSA_HISTORY
    { "/api/history",          HTTP_GET,  get_history },
#endif
#if CONFIG_UPSA_TLS
    { "/api/tls/ca",           HTTP_GET,  get_tls_ca },
    { "/api/tls",              HTTP_POST, post_tls },
#endif
    { "/*",                    HTTP_GET,  get_catchall },
};
_Static_assert(sizeof(ROUTES) / sizeof(ROUTES[0]) == ROUTE_COUNT,
               "ROUTE_COUNT sizes the handler table; adding a route without "
               "updating it is how this boot-looped");

static void register_routes(httpd_handle_t server)
{
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        httpd_uri_t u = {
            .uri = ROUTES[i].uri, .method = ROUTES[i].method,
            .handler = route, .user_ctx = (void *)ROUTES[i].fn,
        };
        /* NOT ESP_ERROR_CHECK. A route that will not register is a missing
         * page; aborting here turns that into a device that never boots, which
         * is how this shipped as a boot loop. Say which one failed and carry
         * on -- a web UI short one endpoint still lets someone reach the rest
         * and reflash. */
        esp_err_t rerr = httpd_register_uri_handler(server, &u);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s -- continuing without it",
                     ROUTES[i].uri, esp_err_to_name(rerr));
        }
    }
}

#if CONFIG_UPSA_TLS
/* The HTTPS server's task stack. Internal RAM, as the plain server's is:
 * handlers write NVS, which a PSRAM stack may not. Measured on the first
 * Rev B board (2026-10-10) at 9 KB: 3.9 KB used at the deepest, through
 * handshakes, /api/scan and every page route; the plain server's deepest is
 * 3.3 KB of its 6.5. 7 KB keeps the same 3 KB of margin. /api/debug's
 * stack_left, asked over HTTPS, reads it. */
#define HTTPS_STACK 7168

static esp_err_t https_start(void)
{
    const uint8_t *crt, *key;
    size_t crt_len, key_len;
    if (!tlscert_get(&crt, &crt_len, &key, &key_len)) return ESP_ERR_INVALID_STATE;
    httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT();
    c.httpd.uri_match_fn = httpd_uri_match_wildcard;
    c.httpd.max_uri_handlers = ROUTE_COUNT + 4;
    c.httpd.stack_size = HTTPS_STACK;
    /* Each open TLS session holds its own buffers (PSRAM, but still); a
     * page needs one or two. The least recently used goes first. */
    c.httpd.max_open_sockets = 3;
    c.httpd.lru_purge_enable = true;
    c.servercert = crt;
    c.servercert_len = crt_len;
    c.prvtkey_pem = key;            /* DER: mbedTLS takes either */
    c.prvtkey_len = key_len;
    c.tls_handshake_timeout_ms = 10000;
    httpd_handle_t h = NULL;
    esp_err_t err = httpd_ssl_start(&h, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server would not start: %s", esp_err_to_name(err));
        return err;
    }
    register_routes(h);
    s_https = h;
    return ESP_OK;
}

/* Loads (or, the first time, makes) the certificates and starts HTTPS; and
 * later, when the board's address or name has moved, issues a new server
 * certificate and restarts HTTPS on it. A task of its own, created for each
 * job and gone after: key generation wants a stack the main task does not
 * have, and NVS wants it in internal RAM. */
static volatile bool s_tls_busy;        /* a tls task is running: do not start another */

static void tls_task(void *arg)
{
    bool first = arg != NULL;
    if (first) {
        /* The first certificate should name the address the board is on;
         * a minute, and then without it (a later check adds it). */
        for (int i = 0; i < 60 && !strcmp(netmgr_ip(), "0.0.0.0"); i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        int64_t t0 = esp_timer_get_time();
        if (tlscert_load() == ESP_OK && https_start() == ESP_OK) {
            ESP_LOGI(TAG, "HTTPS on port 443, https://%s.local/ (%lld ms)%s", netmgr_hostname(),
                     (long long)((esp_timer_get_time() - t0) / 1000),
                     s_require ? "; port 80 redirects to it" : "");
        }
    } else if (tlscert_load() == ESP_OK) {      /* a no-op once loaded; retries a failed load */
        bool fresh = tlscert_refresh();
        if (s_https && fresh) {
            httpd_handle_t old = s_https;
            s_https = NULL;
            httpd_ssl_stop(old);
            https_start();
        } else if (!s_https && https_start() == ESP_OK) {
            /* A start that failed at boot (memory, sockets while Tailscale came
             * up) or after a renewal is tried again on every check. */
            ESP_LOGI(TAG, "HTTPS on port 443 after a retry");
        }
    }
    ESP_LOGI(TAG, "tls task stack headroom %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_tls_busy = false;
    vTaskDelete(NULL);
}

/* Every ten minutes: has the address or name moved, or the end come near? */
static void tls_check(void *arg)
{
    (void)arg;
    if (!s_tls_busy && (!s_https || tlscert_stale())) {
        s_tls_busy = true;
        if (xTaskCreate(tls_task, "tls", 8192, NULL, 3, NULL) != pdPASS) s_tls_busy = false;
    }
}

static void tls_begin(void)
{
    nvs_handle_t h;
    uint8_t rq = 0;
    if (nvs_open("tls", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "require", &rq);
        nvs_close(h);
    }
    s_require = rq != 0;
    s_tls_busy = true;
    if (xTaskCreate(tls_task, "tls", 8192, (void *)1, 3, NULL) != pdPASS) {
        s_tls_busy = false;             /* tls_check will try again */
        ESP_LOGE(TAG, "no memory to start HTTPS");
    }
    const esp_timer_create_args_t ta = { .callback = tls_check, .name = "tls_check" };
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 10LL * 60 * 1000 * 1000);
    }
}
#endif

esp_err_t webui_start(void)
{
    s_req_lock = xSemaphoreCreateMutex();
    if (!s_req_lock) return ESP_ERR_NO_MEM;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    /* The default is 8 and this serves more than 24. Exceeding it used to
     * abort inside ESP_ERROR_CHECK, which boot-looped the device -- it is set
     * from the table rather than a round number so adding a route cannot
     * reintroduce that. */
    cfg.max_uri_handlers = ROUTE_COUNT + 4;
    /* The default 4 KB is not enough. get_status alone holds a ups_data_t, an
     * ota_status_t and a 1.5 KB JSON buffer, and get_scan holds sixteen AP
     * records -- measured as a stack overflow in task httpd that reset the
     * board within seconds of ANY request arriving, including the status
     * page's own three-second poll. It grew that way one endpoint at a time
     * and nothing complained until a UPS was attached and the responses got
     * longer.
     *
     * 8 KB was then generous: measured on v0.08 under page, status and
     * /metrics load, the deepest it went was 3.7 KB. 6.5 KB keeps get_scan's
     * sixteen AP records and 2 KB of margin on top of that. */
    cfg.stack_size = 6656;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    register_routes(server);

    if (netmgr_state() == NETMGR_AP_PROVISIONING) {
        ESP_LOGI(TAG, "setup page at http://192.168.4.1/ on \"%s\"",
                 netmgr_ap_ssid());
    } else {
        ESP_LOGI(TAG, "web UI on port 80, http://%s.local/", netmgr_hostname());
#if CONFIG_UPSA_TLS
        /* Not in setup mode: the setup page is the board's own access
         * point, and a phone joining it expects plain HTTP. */
        tls_begin();
#endif
    }
    return ESP_OK;
}
