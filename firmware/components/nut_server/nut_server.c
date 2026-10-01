/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ota.h"
#include "nut_server.h"
#include "ups_hid.h"
#include "auth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
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

/* No status to report: no UPS, one not yet read, one the board cannot read,
 * or one usb_guard.h has stopped reading. A real NUT driver in that state
 * makes upsd answer DATA-STALE, which upsmon treats as lost communication and
 * Home Assistant as unavailable. This used to publish "OFF", and a UPS that
 * stays OFF past upsmon's OFFDURATION is treated as critical: a host could be
 * shut down because the board could not read its UPS. */
static bool stale(const ups_data_t *d)
{
    /* NUT's own status where this UPS has it: empty means no power state,
     * which NUT's driver reports as stale too. Else the fixed flags. */
    char st[64], al[8];
    const char *why;
    if (ups_hid_nut_status(st, sizeof(st), al, sizeof(al), &why)) return st[0] == '\0';
    return d->status == UPS_STATUS_UNKNOWN;
}

static void nut_status_string(const ups_data_t *d, char *out, size_t cap)
{
    out[0] = '\0';
    char al[8];
    const char *why;
    if (ups_hid_nut_status(out, cap, al, sizeof(al), &why)) return;   /* NUT's own */
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
/* True if NUT's own table for this UPS has `name` with a value. Where it does,
 * that one is published and the fixed one below is not: the table's row is
 * the exact path NUT reads, with NUT's conversions (a CyberPower's battery
 * voltage and frequency corrections, its charge clamp), where the fixed set
 * matches more loosely and converts nothing. */
static bool in_table(const char *name)
{
    char v[4];
    return ups_hid_var_find(name, v, sizeof(v));
}

static int build_vars(const ups_data_t *d, char *buf, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char status[64];
    nut_status_string(d, status, sizeof(status));

    int n = 0;
    n += emit_str(buf + n, cap - n, "device.type", "ups");
    n += emit_str(buf + n, cap - n, "driver.name", "ups-esp32");
    n += emit_str(buf + n, cap - n, "driver.version", app->version);
    n += emit_str(buf + n, cap - n, "ups.mfr", d->mfr);
    n += emit_str(buf + n, cap - n, "ups.model", d->model);
    n += emit_str(buf + n, cap - n, "ups.serial", d->serial);
    n += emit_str(buf + n, cap - n, "ups.status", status);
    {
        char st[64], alarm[200];
        const char *why = NULL;
        if (ups_hid_nut_status(st, sizeof(st), alarm, sizeof(alarm), &why)) {
            if (alarm[0]) n += emit_str(buf + n, cap - n, "ups.alarm", alarm);
            if (why)      n += emit_str(buf + n, cap - n, "input.transfer.reason", why);
        }
    }
    if (!in_table("ups.beeper.status")) n += emit_str(buf + n, cap - n, "ups.beeper.status", beeper_word(d->beeper));
    if (!in_table("ups.test.result")) n += emit_str(buf + n, cap - n, "ups.test.result", test_word(d->test_result));
    if (!in_table("battery.mfr.date")) n += emit_str(buf + n, cap - n, "battery.mfr.date", d->battery_mfr_date);

    if (!in_table("battery.charge")) n += nut_emit_var(buf + n, cap - n, "battery.charge",        "%.0f", d->battery_charge);
    if (!in_table("battery.runtime")) n += nut_emit_var(buf + n, cap - n, "battery.runtime",       "%.0f", d->battery_runtime);
    if (!in_table("battery.voltage")) n += nut_emit_var(buf + n, cap - n, "battery.voltage",       "%.2f", d->battery_voltage);
    if (!in_table("battery.charge.low")) n += nut_emit_var(buf + n, cap - n, "battery.charge.low",    "%.0f", d->lowbatt_limit);
    if (!in_table("input.voltage")) n += nut_emit_var(buf + n, cap - n, "input.voltage",         "%.1f", d->input_voltage);
    if (!in_table("input.frequency")) n += nut_emit_var(buf + n, cap - n, "input.frequency",       "%.1f", d->input_frequency);
    if (!in_table("output.voltage")) n += nut_emit_var(buf + n, cap - n, "output.voltage",        "%.1f", d->output_voltage);
    if (!in_table("ups.load")) n += nut_emit_var(buf + n, cap - n, "ups.load",              "%.0f", d->ups_load);
    if (!in_table("ups.realpower")) n += nut_emit_var(buf + n, cap - n, "ups.realpower",         "%.0f", d->ups_realpower);
    if (!in_table("ups.realpower.nominal")) n += nut_emit_var(buf + n, cap - n, "ups.realpower.nominal", "%.0f", d->ups_realpower_nom);
    if (!in_table("ups.power")) n += nut_emit_var(buf + n, cap - n, "ups.power",             "%.0f", d->ups_apparentpower);
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

/* What one client connection has told us about itself. USERNAME and PASSWORD
 * are remembered as sent and checked only when a write asks for them, as upsd
 * does: a monitor that sends them and only ever reads is never refused. */
typedef struct {
    int sock;
    bool have_user, have_pass;
    char user[AUTH_USER_MAX + 1];
    char pass[AUTH_PASS_MAX + 1];
    /* Reply buffers, on the heap, one pair per connection. They were on
     * nut_cli's 5 KB stack (1400 + 1400 bytes in handle_line's frame), and
     * the NUT-table variables' few extra locals took LIST VAR over the edge:
     * a stack-end watchpoint build caught "Stack canary watchpoint triggered
     * (nut_cli)" on the first LIST VAR; a normal build instead corrupted the
     * heap below the stack and asserted in malloc a moment later. */
    char *buf;
    char *all;
} nut_conn_t;
#define NUT_BUF 1400

/* INSTCMD and SET VAR go through here. With the optional login off (the
 * default) everything is allowed, as before; with it on, the connection's
 * USERNAME and PASSWORD must match it. */
static bool nut_may_write(nut_conn_t *c)
{
    switch (auth_nut_write(c->have_user ? c->user : NULL, c->have_pass ? c->pass : NULL)) {
    case AUTHC_NUT_ALLOW:             return true;
    case AUTHC_NUT_USERNAME_REQUIRED: sendall(c->sock, "ERR USERNAME-REQUIRED\n"); break;
    case AUTHC_NUT_PASSWORD_REQUIRED: sendall(c->sock, "ERR PASSWORD-REQUIRED\n"); break;
    default:                          sendall(c->sock, "ERR ACCESS-DENIED\n"); break;
    }
    return false;
}

static void handle_line(nut_conn_t *c, char *line)
{
    int sock = c->sock;
    /* A NUT client being answered is evidence a fresh image works, for the
     * OTA confirm gate, exactly as a page view is. */
    ota_note_request_served();
    char *a[5];
    int argc = split(line, a, 5);
    if (argc == 0) return;

    for (char *p = a[0]; *p; p++) *p = toupper((unsigned char)*p);

    ups_data_t d;
    ups_hid_get(&d);
    char *buf = c->buf;

    /* Anything addressed to a UPS name we do not have gets NUT's own error,
     * which clients understand, rather than a silent nothing. */
    bool named_ok = (argc >= 3 && strcmp(a[2], NUT_UPS_NAME) == 0);

    if (!strcmp(a[0], "VER")) {
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(buf, NUT_BUF,
                 "UPS to ESP32 Module %s - NUT compatible - http://networkupstools.org/\n",
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
        /* Always OK, as upsd answers: the check happens at INSTCMD or SET VAR
         * (nut_may_write), so a client that volunteers credentials and only
         * reads -- Synology sends monuser/secret -- is never refused. Each may
         * be given once per connection, also as upsd has it. */
        bool is_user = a[0][0] == 'U';
        size_t cap = is_user ? sizeof(c->user) : sizeof(c->pass);
        char *dst = is_user ? c->user : c->pass;
        bool *have = is_user ? &c->have_user : &c->have_pass;
        if (argc < 2) {
            sendall(sock, "ERR INVALID-ARGUMENT\n");
        } else if (*have) {
            sendall(sock, is_user ? "ERR ALREADY-SET-USERNAME\n" : "ERR ALREADY-SET-PASSWORD\n");
        } else {
            strncpy(dst, a[1], cap - 1);
            dst[cap - 1] = '\0';
            *have = true;
            sendall(sock, "OK\n");
        }

    } else if (!strcmp(a[0], "LOGIN")) {
        sendall(sock, argc >= 2 && !strcmp(a[1], NUT_UPS_NAME)
                      ? "OK\n" : "ERR UNKNOWN-UPS\n");

    } else if (!strcmp(a[0], "LOGOUT")) {
        sendall(sock, "OK Goodbye\n");

    } else if (!strcmp(a[0], "LIST") && argc >= 2 && !strcmp(a[1], "UPS")) {
        snprintf(buf, NUT_BUF,
                 "BEGIN LIST UPS\nUPS %s \"%s %s\"\nEND LIST UPS\n",
                 NUT_UPS_NAME,
                 d.mfr[0] ? d.mfr : "USB", d.model[0] ? d.model : "UPS");
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        if (stale(&d)) { sendall(sock, "ERR DATA-STALE\n"); return; }
        snprintf(buf, NUT_BUF, "BEGIN LIST VAR %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        build_vars(&d, buf, NUT_BUF);
        sendall(sock, buf);
        /* Then everything NUT's own table for this UPS found (ups_vartab.h),
         * a line at a time so the list costs no stack however long it is.
         * The fixed set above wins where both have a name. */
        size_t nv = ups_hid_var_count();
        for (size_t i = 0; i < nv; i++) {
            const char *vn;
            char vv[40], line[128], key[80];
            if (!ups_hid_var_get(i, &vn, vv, sizeof(vv))) continue;
            snprintf(key, sizeof(key), "VAR %s %s ", NUT_UPS_NAME, vn);
            if (strstr(buf, key)) continue;
            snprintf(line, sizeof(line), "VAR %s %s \"%s\"\n", NUT_UPS_NAME, vn, vv);
            sendall(sock, line);
        }
        snprintf(buf, NUT_BUF, "END LIST VAR %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CMD")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        snprintf(buf, NUT_BUF, "BEGIN LIST CMD %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!(d.caps & NUT_CMDS[i].cap)) continue;   /* only what it can do */
            snprintf(buf, NUT_BUF, "CMD %s %s\n", NUT_UPS_NAME, NUT_CMDS[i].name);
            sendall(sock, buf);
        }
        snprintf(buf, NUT_BUF, "END LIST CMD %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "RW")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        snprintf(buf, NUT_BUF, "BEGIN LIST RW %s\n", NUT_UPS_NAME);
        sendall(sock, buf);
        if ((d.caps & UPS_CAP_LOWBATT) && ups_valid(d.lowbatt_limit)) {
            snprintf(buf, NUT_BUF, "RW %s battery.charge.low \"%.0f\"\n",
                     NUT_UPS_NAME, d.lowbatt_limit);
            sendall(sock, buf);
        }
        snprintf(buf, NUT_BUF, "END LIST RW %s\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CLIENT")) {
        snprintf(buf, NUT_BUF, "BEGIN LIST CLIENT %s\nEND LIST CLIENT %s\n",
                 NUT_UPS_NAME, NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(sock, "ERR UNKNOWN-UPS\n"); return; }
        if (stale(&d)) { sendall(sock, "ERR DATA-STALE\n"); return; }
        /* Built from the same function as LIST VAR, so a variable can never
         * appear in one and not the other. */
        char *all = c->all;
        build_vars(&d, all, NUT_BUF);
        char want[80];
        int wn = snprintf(want, sizeof(want), "VAR %s %s ", NUT_UPS_NAME, a[3]);
        char *hit = strstr(all, want);
        if (!hit) {
            char vv[40];
            if (ups_hid_var_find(a[3], vv, sizeof(vv))) {
                snprintf(buf, NUT_BUF, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, a[3], vv);
                sendall(sock, buf);
            } else {
                sendall(sock, "ERR VAR-NOT-SUPPORTED\n");
            }
            return;
        }
        char *end = strchr(hit, '\n');
        size_t len = end ? (size_t)(end - hit + 1) : strlen(hit);
        (void)wn;
        send(sock, hit, len, 0);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "DESC")) {
        const char *desc = "Description unavailable";
        for (size_t i = 0; i < sizeof(NUT_DESCS) / sizeof(NUT_DESCS[0]); i++) {
            if (!strcmp(NUT_DESCS[i].name, a[3])) { desc = NUT_DESCS[i].desc; break; }
        }
        snprintf(buf, NUT_BUF, "DESC %s %s \"%s\"\n", NUT_UPS_NAME, a[3], desc);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "UPSDESC")) {
        snprintf(buf, NUT_BUF, "UPSDESC %s \"%s %s\"\n", NUT_UPS_NAME,
                 d.mfr[0] ? d.mfr : "USB", d.model[0] ? d.model : "UPS");
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "NUMLOGINS")) {
        snprintf(buf, NUT_BUF, "NUMLOGINS %s 1\n", NUT_UPS_NAME);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "CMDDESC")) {
        snprintf(buf, NUT_BUF, "CMDDESC %s %s \"%s\"\n",
                 NUT_UPS_NAME, a[3], a[3]);
        sendall(sock, buf);

    } else if (!strcmp(a[0], "INSTCMD") && argc >= 3) {
        const nut_cmd_t *cmd = NULL;
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!strcmp(NUT_CMDS[i].name, a[2])) { cmd = &NUT_CMDS[i]; break; }
        }
        if (strcmp(a[1], NUT_UPS_NAME)) sendall(sock, "ERR UNKNOWN-UPS\n");
        else if (!cmd)                  sendall(sock, "ERR CMD-NOT-SUPPORTED\n");
        else if (!nut_may_write(c))     { /* refusal already sent */ }
        else {
            /* No INSTCMD here takes an argument. load.off carries an optional
             * delay in NUT; this passes 0, meaning immediately, which is what
             * `upscmd ups load.off` with no argument asks for. (This used to
             * read `c->cmd == UPS_CMD_LOAD_OFF ? 0 : 0` -- a ternary with the
             * same value in both branches, which looked like it handled the
             * delay and did nothing at all.) */
            esp_err_t err = ups_hid_command(cmd->cmd, 0);
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
        if (!nut_may_write(c)) return;
        esp_err_t err = ups_hid_command(UPS_CMD_LOWBATT_LIMIT, atoi(a[4]));
        sendall(sock, err == ESP_OK              ? "OK\n"
                    : err == ESP_ERR_INVALID_ARG ? "ERR INVALID-ARGUMENT\n"
                                                 : "ERR SET-FAILED\n");

    } else {
        sendall(sock, "ERR UNKNOWN-COMMAND\n");
    }
}

static void nut_client_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    char line[NUT_LINE_MAX];
    nut_conn_t conn = { .sock = sock };
    conn.buf = malloc(NUT_BUF);
    conn.all = malloc(NUT_BUF);
    if (!conn.buf || !conn.all) {
        ESP_LOGE(TAG, "no memory for a client; closing it");
        free(conn.buf);
        free(conn.all);
        s_clients--;
        close(sock);
        vTaskDelete(NULL);
        return;
    }

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
                ESP_LOGD(TAG, "<- %s", strncasecmp(p, "PASSWORD", 8) ? p : "PASSWORD (hidden)");
                handle_line(&conn, p);
            }
            p = nl ? nl + 1 : NULL;
            while (p && (*p == '\r' || *p == '\n')) p++;
        }
    }

    free(conn.buf);
    free(conn.all);
    memset(&conn, 0, sizeof(conn));        /* the password was on this stack */
    s_clients--;
    /* Headroom left on this task's stack at its deepest, in bytes: a real
     * number to size it by, after it overflowed once. */
    ESP_LOGI(TAG, "client disconnected (%d left, stack headroom %u)", (int)s_clients,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
        /* 5 KB, not 4: two clients listing every variable took a 4 KB stack
         * to within 64 bytes of overflowing (memdiag, v0.08) -- a 1.4 KB reply
         * buffer, the UPS snapshot, and newlib formatting floats, which is
         * stack-hungry. One more nested call would have crashed the board. */
        if (xTaskCreate(nut_client_task, "nut_cli", 5120,
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
