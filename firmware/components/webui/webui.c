/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "webui.h"
#include "ups_hid.h"
#include "netmgr.h"
#include "metrics.h"
#include "fleet.h"
#include "crashlog.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nut_server.h"
#include "ota.h"
#include "nvs.h"
#include "board.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "webui";

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
    static char buf[2048];
    /* Escape scratch, static for the same reason as buf. Two bytes per input
     * byte is the worst case for a quote-heavy string, plus a terminator. */
    static char e1[200], e2[200], e3[200], e4[200], e5[24];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{");
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

    n += snprintf(buf + n, sizeof(buf) - n,
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
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"wifi\":{\"state\":%d,\"ip\":\"%s\",\"ap\":\"%s\","
                  "\"ssid\":\"%s\",\"rssi\":%d,",
                  (int)netmgr_state(), jesc(netmgr_ip(), e1, sizeof(e1)),
                  jesc(netmgr_ap_ssid(), e2, sizeof(e2)),
                  jesc(netmgr_ssid(), e3, sizeof(e3)), netmgr_rssi());
    /* The setup join: its progress, the address it was given and why it
     * failed. The setup page shows the address before the AP goes away,
     * which is the only moment a person can be told it without a router. */
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"host\":\"%s\",\"join\":%d,\"join_ip\":\"%s\","
                  "\"join_err\":\"%s\"},",
                  jesc(netmgr_hostname(), e1, sizeof(e1)), (int)netmgr_join_state(),
                  jesc(netmgr_join_ip(), e2, sizeof(e2)),
                  jesc(netmgr_join_error(), e3, sizeof(e3)));
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"nut\":{\"port\":%u,\"name\":\"%s\",\"clients\":%d},"
                  "\"ups\":{\"attached\":%s,\"mfr\":\"%s\",\"model\":\"%s\","
                  "\"vid\":\"%04x\",\"pid\":\"%04x\",\"status\":%lu,",
                  NUT_DEFAULT_PORT, jesc(nut_server_ups_name(), e1, sizeof(e1)),
                  nut_server_clients(),
                  d.attached ? "true" : "false", jesc(d.mfr, e2, sizeof(e2)),
                  jesc(d.model, e3, sizeof(e3)),
                  d.vid, d.pid, (unsigned long)d.status);
    n += json_num(buf + n, sizeof(buf) - n, "battery_charge",  d.battery_charge,  false);
    n += json_num(buf + n, sizeof(buf) - n, "battery_runtime", d.battery_runtime, false);
    n += json_num(buf + n, sizeof(buf) - n, "input_voltage",   d.input_voltage,   false);
    n += json_num(buf + n, sizeof(buf) - n, "output_voltage",  d.output_voltage,  false);
    n += json_num(buf + n, sizeof(buf) - n, "load",            d.ups_load,        false);
    n += json_num(buf + n, sizeof(buf) - n, "battery_voltage", d.battery_voltage, false);
    n += json_num(buf + n, sizeof(buf) - n, "input_frequency", d.input_frequency, false);
    n += json_num(buf + n, sizeof(buf) - n, "realpower",       d.ups_realpower,   false);
    n += json_num(buf + n, sizeof(buf) - n, "realpower_nom",   d.ups_realpower_nom, false);
    n += json_num(buf + n, sizeof(buf) - n, "apparentpower",   d.ups_apparentpower, false);
    n += json_num(buf + n, sizeof(buf) - n, "lowbatt_limit",   d.lowbatt_limit,   false);
    /* caps drives which controls the page offers: the UPS's own descriptor
     * decides, not a model name. */
    if (d.battery_mfr_date[0]) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "\"battery_mfr_date\":\"%s\",",
                      jesc(d.battery_mfr_date, e1, sizeof(e1)));
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "\"battery_mfr_date\":null,");
    }
    /* runtime_age_s: how long the UPS's runtime figure has gone unchanged,
     * -1 if never reported. desc: the report descriptor's fingerprint. */
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"runtime_age_s\":%lld,\"desc_hash\":\"%s\",\"desc_len\":%u,",
                  (long long)(d.runtime_changed_us
                              ? (esp_timer_get_time() - d.runtime_changed_us) / 1000000 : -1),
                  jesc(d.desc_hash, e1, sizeof(e1)), (unsigned)d.desc_len);
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"caps\":%lu,\"beeper\":%u,\"test_result\":%u",
                  (unsigned long)d.caps, d.beeper, d.test_result);
    ota_status_t o;
    ota_get(&o);
    n += snprintf(buf + n, sizeof(buf) - n,
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
    n += snprintf(buf + n, sizeof(buf) - n, "\"last_crash\":\"%s\",",
                  jesc(crashlog_last(), e1, sizeof(e1)));
    /* The fleet heartbeat: whether it is built in, what the owner allows, and
     * how the last one went. last_ok is seconds ago, -1 if never this boot. */
    fleet_status_t f;
    fleet_get(&f);
    int64_t now_s = esp_timer_get_time() / 1000000;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"fleet\":{\"configured\":%s,\"enabled\":%s,\"stats\":%s,\"stats_auto\":%s,"
                  "\"channel\":\"%s\",\"sent\":%u,\"last_ok\":%lld,\"error\":\"%s\"}}",
                  f.configured ? "true" : "false", f.enabled ? "true" : "false",
                  f.stats ? "true" : "false", f.stats_auto ? "true" : "false",
                  jesc(f.channel, e2, sizeof(e2)),
                  (unsigned)f.sent,
                  (long long)(f.last_ok_s < 0 ? -1 : now_s - f.last_ok_s),
                  jesc(f.last_error, e1, sizeof(e1)));

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

static esp_err_t post_ups_reset(httpd_req_t *req)
{
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
    char body[448];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

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

    bool setup = netmgr_state() == NETMGR_AP_PROVISIONING;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
        err = netmgr_provision(ssid->valuestring,
                               cJSON_IsString(pass) ? pass->valuestring : "", &ip);
    }
    cJSON_Delete(root);

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
    netmgr_ap_t aps[16];
    int n = netmgr_scan(aps, sizeof(aps) / sizeof(aps[0]));

    httpd_resp_set_type(req, "application/json");
    if (n < 0) {
        return httpd_resp_send(req, "{\"ok\":false,\"nets\":[]}",
                               HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"nets\":[");
    for (int i = 0; i < n; i++) {
        char item[96];
        /* The SSID goes through cJSON's escaper: an apostrophe or a backslash
         * in a network name would otherwise produce JSON the page cannot
         * parse, and people do name networks like that. */
        cJSON *str = cJSON_CreateString(aps[i].ssid);
        char *esc = str ? cJSON_PrintUnformatted(str) : NULL;
        snprintf(item, sizeof(item), "%s{\"ssid\":%s,\"rssi\":%d,\"secure\":%s}",
                 i ? "," : "", esc ? esc : "\"?\"", aps[i].rssi,
                 aps[i].secure ? "true" : "false");
        cJSON_free(esc);
        cJSON_Delete(str);
        httpd_resp_sendstr_chunk(req, item);
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
    char body[160];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

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

/* The offered release's abridged notes, for the update card. A route of its
 * own rather than part of /api/status: it is up to a kilobyte, it changes only
 * when a check finds something new, and the page fetches it once per release
 * rather than every three seconds. Built with cJSON because the text is a
 * person's, from a tag message, and may hold anything. */
static esp_err_t get_ota_notes(httpd_req_t *req)
{
    static char tag[48], url[160], notes[1024];
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
    char body[96];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

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

/* The address: what is stored, what is in use, and whether a fixed address
 * failed its gateway check this session. */
static esp_err_t get_network(httpd_req_t *req)
{
    netmgr_ipcfg_t c, cur;
    bool fell_back;
    netmgr_ipcfg_get(&c, &fell_back);
    netmgr_ipcfg_current(&cur);
    static char out[512];
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
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';
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
static esp_err_t get_debug(httpd_req_t *req)
{
    cJSON *o = fleet_diag();
    if (!o) o = cJSON_CreateObject();
    if (!o) { httpd_resp_send_500(req); return ESP_FAIL; }
    uint8_t mac[6];
    char s[40];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "mac", s);
    cJSON_AddNumberToObject(o, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(o, "tasks", uxTaskGetNumberOfTasks());
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
    char body[64];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

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

static esp_err_t post_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(TAG, "reboot requested over HTTP");
    vTaskDelay(pdMS_TO_TICKS(400));      /* let the reply leave first */
    esp_restart();
    return ESP_OK;
}

/* Not served here: a factory reset. Clearing the network is what makes a board
 * in a closet unreachable, so the only way to ask for it is to be standing in
 * front of it, pressing BOOT five times -- see factory_gesture_task in main.c. Firmware
 * update lives under /api/ota (check, install, auto) and pulls a published
 * release rather than accepting an uploaded image. */

/* One place that knows how many routes there are, used both to size the
 * server's handler table and to walk it. */
#define ROUTE_COUNT 18

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    /* The default is 8 and this serves 18. Exceeding it used to abort inside
     * ESP_ERROR_CHECK below, which boot-looped the device -- it is set from the
     * table rather than a round number so adding a route cannot reintroduce
     * that. */
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

    /* Order matters: the server tries these in turn and the wildcard route
     * matches everything, so it has to be last or it swallows the API. */
    static const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = get_root },
        { .uri = "/api/status",    .method = HTTP_GET,  .handler = get_status },
        { .uri = "/metrics",       .method = HTTP_GET,  .handler = get_metrics },
        { .uri = "/api/scan",      .method = HTTP_GET,  .handler = get_scan },
        { .uri = "/api/descriptor", .method = HTTP_GET, .handler = get_descriptor },
        { .uri = "/api/wifi",      .method = HTTP_POST, .handler = post_wifi },
        { .uri = "/api/ups/reset", .method = HTTP_POST, .handler = post_ups_reset },
        { .uri = "/api/cmd",       .method = HTTP_POST, .handler = post_cmd },
        { .uri = "/api/ota/notes",   .method = HTTP_GET,  .handler = get_ota_notes },
        { .uri = "/api/ota/check",   .method = HTTP_POST, .handler = post_ota_check },
        { .uri = "/api/ota/install", .method = HTTP_POST, .handler = post_ota_install },
        { .uri = "/api/ota/auto",    .method = HTTP_POST, .handler = post_ota_auto },
        { .uri = "/api/fleet",       .method = HTTP_POST, .handler = post_fleet },
        { .uri = "/api/network",     .method = HTTP_GET,  .handler = get_network },
        { .uri = "/api/network",     .method = HTTP_POST, .handler = post_network },
        { .uri = "/api/debug",       .method = HTTP_GET,  .handler = get_debug },
        { .uri = "/api/reboot",      .method = HTTP_POST, .handler = post_reboot },
        { .uri = "/*",             .method = HTTP_GET,  .handler = get_catchall },
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) == ROUTE_COUNT,
                   "ROUTE_COUNT sizes the handler table; adding a route without "
                   "updating it is how this boot-looped");

    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        /* NOT ESP_ERROR_CHECK. A route that will not register is a missing
         * page; aborting here turns that into a device that never boots, which
         * is how this shipped as a boot loop. Say which one failed and carry
         * on -- a web UI short one endpoint still lets someone reach the rest
         * and reflash. */
        esp_err_t rerr = httpd_register_uri_handler(server, &routes[i]);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s -- continuing without it",
                     routes[i].uri, esp_err_to_name(rerr));
        }
    }

    if (netmgr_state() == NETMGR_AP_PROVISIONING) {
        ESP_LOGI(TAG, "setup page at http://192.168.4.1/ on \"%s\"",
                 netmgr_ap_ssid());
    } else {
        ESP_LOGI(TAG, "web UI on port 80, http://%s.local/", netmgr_hostname());
    }
    return ESP_OK;
}
