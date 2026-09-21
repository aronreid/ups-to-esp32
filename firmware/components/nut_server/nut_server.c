/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nut_server.h"
#include "ups_hid.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdatomic.h>

static const char *TAG = "nut";

/* How many clients are connected right now. Shown on the status page, because
 * "is my NAS actually talking to this?" is otherwise unanswerable without a
 * packet capture. Written from client tasks and read from the web server's,
 * hence the atomic. */
static _Atomic int s_clients;

#define NUT_MAX_CLIENTS 4
#define NUT_UPS_NAME    "ups"
#define NUT_LINE_MAX    256

/* One variable's worth of NUT output. Fields the UPS did not report are
 * omitted entirely rather than sent as zero -- a NUT client treats a present
 * variable as authoritative. */
static int nut_emit_var(char *buf, size_t cap, const char *name,
                        const char *fmt, float v)
{
    if (!ups_valid(v)) return 0;
    char val[32];
    snprintf(val, sizeof(val), fmt, v);
    return snprintf(buf, cap, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, name, val);
}

static void nut_status_string(const ups_data_t *d, char *out, size_t cap)
{
    out[0] = '\0';
    if (d->status == UPS_STATUS_UNKNOWN) { snprintf(out, cap, "OFF"); return; }
    if (d->status & UPS_STATUS_ONLINE)       strlcat(out, "OL ", cap);
    if (d->status & UPS_STATUS_ONBATT)       strlcat(out, "OB ", cap);
    if (d->status & UPS_STATUS_LOWBATT)      strlcat(out, "LB ", cap);
    if (d->status & UPS_STATUS_CHARGING)     strlcat(out, "CHRG ", cap);
    if (d->status & UPS_STATUS_DISCHARGE)    strlcat(out, "DISCHRG ", cap);
    if (d->status & UPS_STATUS_REPLACEBATT)  strlcat(out, "RB ", cap);
    if (d->status & UPS_STATUS_OVERLOAD)     strlcat(out, "OVER ", cap);
    size_t n = strlen(out);
    if (n && out[n - 1] == ' ') out[n - 1] = '\0';
}

/* The protocol, as clients actually use it.
 *
 * NUT's wire format is public and line-based, so "being a NUT server" means
 * answering these correctly -- none of NUT's own code has to be present, and
 * none of it could run here anyway (its driver forks, chroots, drops
 * privileges and talks libusb).
 *
 * Home Assistant issues LIST UPS then LIST VAR and little else. upsmon wants
 * LOGIN, and gets upset by an error it does not recognise, so the unknowns
 * below answer in NUT's own error vocabulary rather than a generic refusal.
 *
 * Everything is read-only except SET VAR on battery.charge.low and the
 * instant commands, which map onto ups_hid_command().
 */

/* A description per variable, for GET DESC. NUT clients show these in a UI. */
typedef struct {
    const char *name;
    const char *desc;
} nut_desc_t;

static const nut_desc_t NUT_DESCS[] = {
    { "battery.charge",         "Battery charge (percent)" },
    { "battery.runtime",        "Battery runtime (seconds)" },
    { "battery.voltage",        "Battery voltage (V)" },
    { "battery.charge.low",     "Remaining battery level when UPS switches to LB (percent)" },
    { "battery.mfr.date",       "Battery manufacturing date" },
    { "input.voltage",          "Input voltage (V)" },
    { "input.frequency",        "Input frequency (Hz)" },
    { "output.voltage",         "Output voltage (V)" },
    { "ups.load",               "Load on UPS (percent)" },
    { "ups.realpower",          "Current value of real power (W)" },
    { "ups.realpower.nominal",  "UPS real power rating (W)" },
    { "ups.power",              "Current value of apparent power (VA)" },
    { "ups.status",             "UPS status" },
    { "ups.mfr",                "UPS manufacturer" },
    { "ups.model",              "UPS model" },
    { "ups.serial",             "UPS serial number" },
    { "ups.test.result",        "Results of last self test" },
    { "ups.beeper.status",      "UPS beeper status" },
    { "device.type",            "Device type (ups)" },
    { "driver.name",            "Driver name" },
    { "driver.version",         "Driver version" },
};

/* Instant commands, in NUT's namespace. The UPS's own descriptor decides
 * which of these are advertised: LIST CMD only reports the ones whose
 * capability bit is set, exactly as the web UI only draws those buttons. */
typedef struct {
    const char *name;
    ups_cmd_t   cmd;
    uint32_t    cap;
} nut_cmd_t;

static const nut_cmd_t NUT_CMDS[] = {
    { "test.battery.start.quick", UPS_CMD_TEST_QUICK,     UPS_CAP_TEST },
    { "test.battery.start.deep",  UPS_CMD_TEST_DEEP,      UPS_CAP_TEST },
    { "test.battery.stop",        UPS_CMD_TEST_ABORT,     UPS_CAP_TEST },
    { "beeper.disable",           UPS_CMD_BEEPER_DISABLE, UPS_CAP_BEEPER },
    { "beeper.enable",            UPS_CMD_BEEPER_ENABLE,  UPS_CAP_BEEPER },
    { "beeper.mute",              UPS_CMD_BEEPER_MUTE,    UPS_CAP_BEEPER },
    { "load.off",                 UPS_CMD_LOAD_OFF,       UPS_CAP_SHUTDOWN },
};

static const char *beeper_word(uint8_t b)
{
    switch (b) {
    case UPS_BEEPER_DISABLED: return "disabled";
    case UPS_BEEPER_ENABLED:  return "enabled";
    case UPS_BEEPER_MUTED:    return "muted";
    default:                  return NULL;
    }
}

/* NUT's own wording for ups.test.result, so `upsc` reads the same here as it
 * would against usbhid-ups. */
static const char *test_word(uint8_t t)
{
    static const char *w[] = { "No test", "Done and passed", "Done and warning",
                               "Done and error", "Aborted", "In progress",
                               "No test initiated" };
    return (t < sizeof(w) / sizeof(w[0])) ? w[t] : NULL;
}

static void sendall(int sock, const char *s) { send(sock, s, strlen(s), 0); }

static int emit_str(char *buf, size_t cap, const char *name, const char *val)
{
    if (!val || !val[0]) return 0;
    return snprintf(buf, cap, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, name, val);
}

/* Every variable this UPS currently has a value for. Used by LIST VAR, and
 * scanned by GET VAR so the two can never disagree. */
static int build_vars(const ups_data_t *d, char *buf, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char status[64];
    nut_status_string(d, status, sizeof(status));

    int n = 0;
    n += emit_str(buf + n, cap - n, "device.type", "ups");
    n += emit_str(buf + n, cap - n, "driver.name", "ups-adaptor");
    n += emit_str(buf + n, cap - n, "driver.version", app->version);
    n += emit_str(buf + n, cap - n, "ups.mfr", d->mfr);
    n += emit_str(buf + n, cap - n, "ups.model", d->model);
    n += emit_str(buf + n, cap - n, "ups.serial", d->serial);
    n += emit_str(buf + n, cap - n, "ups.status", status);
    n += emit_str(buf + n, cap - n, "ups.beeper.status", beeper_word(d->beeper));
    n += emit_str(buf + n, cap - n, "ups.test.result", test_word(d->test_result));
    n += emit_str(buf + n, cap - n, "battery.mfr.date", d->battery_mfr_date);

    n += nut_emit_var(buf + n, cap - n, "battery.charge",        "%.0f", d->battery_charge);
    n += nut_emit_var(buf + n, cap - n, "battery.runtime",       "%.0f", d->battery_runtime);
    n += nut_emit_var(buf + n, cap - n, "battery.voltage",       "%.2f", d->battery_voltage);
    n += nut_emit_var(buf + n, cap - n, "battery.charge.low",    "%.0f", d->lowbatt_limit);
    n += nut_emit_var(buf + n, cap - n, "input.voltage",         "%.1f", d->input_voltage);
    n += nut_emit_var(buf + n, cap - n, "input.frequency",       "%.1f", d->input_frequency);
    n += nut_emit_var(buf + n, cap - n, "output.voltage",        "%.1f", d->output_voltage);
    n += nut_emit_var(buf + n, cap - n, "ups.load",              "%.0f", d->ups_load);
    n += nut_emit_var(buf + n, cap - n, "ups.realpower",         "%.0f", d->ups_realpower);
    n += nut_emit_var(buf + n, cap - n, "ups.realpower.nominal", "%.0f", d->ups_realpower_nom);
    n += nut_emit_var(buf + n, cap - n, "ups.power",             "%.0f", d->ups_apparentpower);
    return n;
}

/* Split a command line into at most 4 words, honouring "quoted strings" --
 * SET VAR carries values that may contain spaces. */
static int split(char *line, char *argv[], int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void handle_line(int sock, char *line)
{
    char *a[5];
    int argc = split(line, a, 5);
    if (argc == 0) return;

    for (char *p = a[0]; *p; p++) *p = toupper((unsigned char)*p);

    ups_data_t d;
    ups_hid_get(&d);
    char buf[1400];

    /* Anything addressed to a UPS name we do not have gets NUT's own error,
     * which clients understand, rather than a silent nothing. */
    bool named_ok = (argc >= 3 && strcmp(a[2], NUT_UPS_NAME) == 0);

    if (!strcmp(a[0], "VER")) {
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(buf, sizeof(buf),
                 "ups-adaptor %s - NUT compatible - http://networkupstools.org/\n",
                 app->version);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "NETVER") || !strcmp(a[0], "PROTVER")) {
        sendall(sock, "1.3\n");

    } else if (!strcmp(a[0], "HELP")) {
        sendall(sock, "Commands: HELP VER NETVER LIST GET SET INSTCMD "
                      "LOGIN LOGOUT USERNAME PASSWORD STARTTLS\n");

    } else if (!strcmp(a[0], "STARTTLS")) {
        sendall(sock, "ERR FEATURE-NOT-SUPPORTED\n");

    } else if (!strcmp(a[0], "USERNAME") || !strcmp(a[0], "PASSWORD")) {
        /* No access control: this device serves read-only data on a LAN, and
         * refusing a credential a client volunteers only breaks the client. */
        sendall(sock, "OK\n");

    } else if (!strcmp(a[0], "LOGIN")) {
        sendall(sock, argc >= 2 && !strcmp(a[1], NUT_UPS_NAME)
                      ? "OK\n" : "ERR UNKNOWN-UPS\n");

    } else if (!strcmp(a[0], "LOGOUT")) {
        sendall(sock, "OK Goodbye\n");

    } else if (!strcmp(a[0], "LIST") && argc >= 2 && !strcmp(a[1], "UPS")) {
        snprintf(buf, sizeof(buf),
                 "BEGIN LIST UPS\nUPS %s \"%s %s\"\nEND LIST UPS\n",
                 NUT_UPS_NAME,
                 d.mfr[0] ? d.mfr : "USB", d.model[0] ? d.model : "UPS");
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        snprintf(buf, sizeof(buf), "BEGIN LIST VAR %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        build_vars(&d, buf, sizeof(buf));
        sendall(sock, buf);
        snprintf(buf, sizeof(buf), "END LIST VAR %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CMD")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        snprintf(buf, sizeof(buf), "BEGIN LIST CMD %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!(d.caps & NUT_CMDS[i].cap)) continue;   /* only what it can do */
            snprintf(buf, sizeof(buf), "CMD %s %s\n", NUT_UPS_NAME, NUT_CMDS[i].name);
            sendall(sock, buf);
        }
        snprintf(buf, sizeof(buf), "END LIST CMD %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "RW")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        snprintf(buf, sizeof(buf), "BEGIN LIST RW %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        if ((d.caps & UPS_CAP_LOWBATT) && ups_valid(d.lowbatt_limit)) {
            snprintf(buf, sizeof(buf), "RW %s battery.charge.low \"%.0f\"\n",
                     NUT_UPS_NAME, d.lowbatt_limit);
            sendall(sock, buf);
        }
        snprintf(buf, sizeof(buf), "END LIST RW %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CLIENT")) {
        snprintf(buf, sizeof(buf), "BEGIN LIST CLIENT %s\nEND LIST CLIENT %s\n",
                 NUT_UPS_NAME, NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        /* Built from the same function as LIST VAR, so a variable can never
         * appear in one and not the other. */
        char all[1400];
        build_vars(&d, all, sizeof(all));
        char want[80];
        int wn = snprintf(want, sizeof(want), "VAR %s %s ", NUT_UPS_NAME, a[3]);
        char *hit = strstr(all, want);
        if (!hit) { sendall(sock, "ERR VAR-NOT-SUPPORTED\n"); return; }
        char *end = strchr(hit, '\n');
        size_t len = end ? (size_t)(end - hit + 1) : strlen(hit);
        (void)wn;
        send(sock, hit, len, 0);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "DESC")) {
        const char *desc = "Description unavailable";
        for (size_t i = 0; i < sizeof(NUT_DESCS) / sizeof(NUT_DESCS[0]); i++) {
            if (!strcmp(NUT_DESCS[i].name, a[3])) { desc = NUT_DESCS[i].desc; break; }
        }
        snprintf(buf, sizeof(buf), "DESC %s %s \"%s\"\n", NUT_UPS_NAME, a[3], desc);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "UPSDESC")) {
        snprintf(buf, sizeof(buf), "UPSDESC %s \"%s %s\"\n", NUT_UPS_NAME,
                 d.mfr[0] ? d.mfr : "USB", d.model[0] ? d.model : "UPS");
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "NUMLOGINS")) {
        snprintf(buf, sizeof(buf), "NUMLOGINS %s 1\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "CMDDESC")) {
        snprintf(buf, sizeof(buf), "CMDDESC %s %s \"%s\"\n",
                 NUT_UPS_NAME, a[3], a[3]);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "INSTCMD") && argc >= 3) {
        const nut_cmd_t *c = NULL;
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!strcmp(NUT_CMDS[i].name, a[2])) { c = &NUT_CMDS[i]; break; }
        }
        if (strcmp(a[1], NUT_UPS_NAME)) sendall(sock, "ERR UNKNOWN-UPS\n");
        else if (!c)                    sendall(sock, "ERR CMD-NOT-SUPPORTED\n");
        else {
            /* load.off carries a delay in NUT; without one, go immediately. */
            esp_err_t err = ups_hid_command(c->cmd,
                                            c->cmd == UPS_CMD_LOAD_OFF ? 0 : 0);
            sendall(sock, err == ESP_OK ? "OK\n"
                        : err == ESP_ERR_NOT_SUPPORTED ? "ERR CMD-NOT-SUPPORTED\n"
                        : "ERR INSTCMD-FAILED\n");
        }

    } else if (!strcmp(a[0], "SET") && argc >= 5 && !strcmp(a[1], "VAR")) {
        if (strcmp(a[2], NUT_UPS_NAME)) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        if (strcmp(a[3], "battery.charge.low")) {
            sendall(sock, "ERR VAR-NOT-SUPPORTED\n");
            return;
        }
        esp_err_t err = ups_hid_command(UPS_CMD_LOWBATT_LIMIT, atoi(a[4]));
        sendall(sock, err == ESP_OK ? "OK\n" : "ERR SET-FAILED\n");

    } else {
        sendall(sock, "ERR UNKNOWN-COMMAND\n");
    }
}

static void nut_client_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    char line[NUT_LINE_MAX];

    for (;;) {
        int n = recv(sock, line, sizeof(line) - 1, 0);
        if (n <= 0) break;
        line[n] = '\0';
        /* A client may pipeline several commands in one segment, and always
         * terminates each with CRLF or LF. Handle them one line at a time. */
        char *p = line;
        while (p && *p) {
            char *nl = strpbrk(p, "\r\n");
            if (nl) *nl = '\0';
            if (*p) {
                ESP_LOGD(TAG, "<- %s", p);
                handle_line(sock, p);
            }
            p = nl ? nl + 1 : NULL;
            while (p && (*p == '\r' || *p == '\n')) p++;
        }
    }

    s_clients--;
    ESP_LOGI(TAG, "client disconnected (%d left)", (int)s_clients);
    close(sock);
    vTaskDelete(NULL);
}

static void nut_listen_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock, NUT_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "bind/listen on %u failed: errno %d", port, errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on %u", port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int sock = accept(listen_sock, (struct sockaddr *)&peer, &plen);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        /* Clients hold the connection open indefinitely; keepalive is what
         * reclaims sockets when a NAS is yanked rather than disconnected. */
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

        ESP_LOGI(TAG, "client from %s", inet_ntoa(peer.sin_addr));
        s_clients++;
        if (xTaskCreate(nut_client_task, "nut_cli", 4096,
                        (void *)(intptr_t)sock, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "out of memory for client, dropping");
            s_clients--;
            close(sock);
        }
    }
}

int nut_server_clients(void) { return (int)s_clients; }

const char *nut_server_ups_name(void) { return NUT_UPS_NAME; }

esp_err_t nut_server_start(uint16_t port)
{
    if (xTaskCreate(nut_listen_task, "nut_listen", 4096,
                    (void *)(uintptr_t)port, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
