/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ota.h"
#include "nut_server.h"
#include "ups_hid.h"
#include "auth.h"
#include "nut_line.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "netmgr.h"
#include "netmgr_ap.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdatomic.h>
#include "sdkconfig.h"
#if CONFIG_UPSA_TLS
#include "esp_tls.h"
#include "tlscert.h"
#endif

static const char *TAG = "nut";

/* How many clients are connected right now. Shown on the status page, because
 * "is my NAS actually talking to this?" is otherwise unanswerable without a
 * packet capture. Written from client tasks and read from the web server's,
 * hence the atomic. */
static _Atomic int s_clients;
/* The address of each live connection (0 = free slot), for NUT_PER_PEER.
 * Taken by the accept loop at hand-off, given back by whichever task ends the
 * connection. */
static _Atomic uint32_t s_peer_addr[8];
typedef struct { int sock; uint32_t peer; } nut_accepted_t;

#define NUT_MAX_CLIENTS 4           /* the listen() backlog, not a cap */
#define NUT_UPS_NAME    "ups"

/* Every board serves NUT from a pool of workers, one client each. A worker is
 * made the first time that many clients are connected at once and is never
 * deleted; the pool is also the cap, and a client beyond it is refused.
 *
 * Why not a task per client, as Rev A had: measured on Rev B (2026-10-11), a
 * task per client cost about 12 KB of internal RAM per STARTTLS client (a
 * 5 KB stack, then a 6 KB one for TLS, and the reply buffers). Ten clients
 * took a single-UPS board to 4.6 KB free; on the three-UPS Tailscale bench
 * build the third STARTTLS client found no 6 KB block at all ("no memory for
 * a TLS client"). Anyone on the LAN can open connections, so with no cap that
 * was also a way to starve the board. And a task with a PSRAM stack that
 * deletes itself goes through vTaskDeleteWithCaps(), which makes a helper
 * task in internal RAM and abort()s if it cannot: a client hanging up could
 * then crash a board short of memory. Workers never delete themselves.
 *
 * With PSRAM the stacks and each client's buffers are there. That is safe
 * because nothing a client reaches writes or reads flash (a task with a
 * PSRAM stack must not run while the cache is off): auth checks an in-memory
 * hash, INSTCMD and SET VAR are queued to ups_task (ups_hid_command ->
 * call_task), readings come from ups_hid's in-RAM snapshot, the certificate
 * is read from RAM (tlscert_get), and OTA only counts the request
 * (ota_note_request_served). Without PSRAM (Rev A) there are fewer workers,
 * in internal RAM, made only as clients arrive. */
#if CONFIG_SPIRAM
#define NUT_WORKERS      6
#define NUT_STACK_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define NUT_WORKERS      3
#define NUT_STACK_CAPS   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
_Static_assert(NUT_WORKERS <= sizeof(s_peer_addr) / sizeof(s_peer_addr[0]),
               "s_peer_addr needs a slot per worker");
#if CONFIG_UPSA_TLS
/* A TLS session runs on top of the plain one on the same worker: measured
 * on the first Rev B board (2026-10-10), 3.2 KB used through the handshake
 * and LIST VAR, LIST CMD, on top of the plain path's frames. The disconnect
 * line logs what is left. */
#define NUT_WORKER_STACK 8192
#else
/* 5 KB, not 4: two clients listing every variable took a 4 KB stack to
 * within 64 bytes of overflowing (memdiag, v0.08) -- a 1.4 KB reply buffer,
 * the UPS snapshot, and newlib formatting floats, which is stack-hungry. */
#define NUT_WORKER_STACK 5120
#endif
static QueueHandle_t s_accepted;
static int s_workers;               /* made so far; only nut_listen touches it */

/* A client's buffers: PSRAM where there is some, internal RAM otherwise. */
static void *conn_calloc(size_t n, size_t size)
{
#if CONFIG_SPIRAM
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return calloc(n, size);
}

/* A client that has gone without closing (a NAS that lost power, a Wi-Fi drop
 * that ate its FIN) is found by keepalive: first probe after a minute idle,
 * gone after five unanswered. lwIP's defaults were two hours and nine, and
 * with 16 sockets shared by NUT, the page and updates, a few such ghosts
 * locked every new client out for that long. */
#define NUT_KEEPIDLE_S   60
#define NUT_KEEPINTVL_S  10
#define NUT_KEEPCNT      5
/* And one that is connected but says nothing at all for this long is let go.
 * upsmon asks every 5 s and Home Assistant every minute. */
#define NUT_IDLE_S       (15 * 60)
/* But a client that has not completed one command in this long is dropped at
 * once: upsmon and Home Assistant send LOGIN or LIST straight away, and an
 * idle socket is what ties up a worker (3 on Rev A) for the full NUT_IDLE_S. */
#define NUT_FIRST_CMD_S  20
/* Connections one address may hold. Two, because Home Assistant and upsmon on
 * one NAS is real; a third is a host trying to fill the pool. */
#define NUT_PER_PEER     2
/* A reply that cannot be sent for this long means the client is not reading
 * (a connection that asks for LIST VAR over and over and never takes the
 * answer would otherwise hold its worker in send() for ever, and Rev A has
 * three). The connection is dropped, which frees the worker. */
#define NUT_SEND_S       10
/* Readings older than this are not served as current, whatever else says
 * they are: ups_task polls every 2 s, so this is a reader that has stopped. */
#define NUT_STALE_US     (60LL * 1000 * 1000)

/* One variable's worth of NUT output. Fields the UPS did not report are
 * omitted entirely rather than sent as zero -- a NUT client treats a present
 * variable as authoritative. */
static int nut_emit_var(char *buf, size_t cap, const char *name,
                        const char *fmt, float v)
{
    if (!ups_valid(v)) return 0;
    char val[32];
    snprintf(val, sizeof(val), fmt, v);
    int n = snprintf(buf, cap, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, name, val);
    return (n < 0) ? 0 : (n >= (int)cap) ? (int)(cap ? cap - 1 : 0) : n;
}

/* No status to report: no UPS, one not yet read, one the board cannot read,
 * or one usb_guard.h has stopped reading. A real NUT driver in that state
 * makes upsd answer DATA-STALE, which upsmon treats as lost communication and
 * Home Assistant as unavailable. This used to publish "OFF", and a UPS that
 * stays OFF past upsmon's OFFDURATION is treated as critical: a host could be
 * shut down because the board could not read its UPS. */
static bool stale(const ups_data_t *d)
{
    /* Not attached: the board has stopped hearing from it. Whatever NUT
     * status was last built is history, not the UPS's state now. */
    if (!d->attached) return true;
    if (d->last_update_us &&
        esp_timer_get_time() - d->last_update_us > NUT_STALE_US) return true;
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

/* "Maker Model", quoted, for LIST UPS and GET UPSDESC. */
static void ups_desc(const ups_data_t *d, char *out, size_t cap)
{
    char raw[sizeof(d->mfr) + sizeof(d->model) + 2];
    snprintf(raw, sizeof(raw), "%s %s", d->mfr[0] ? d->mfr : "USB",
             d->model[0] ? d->model : "UPS");
    nut_quote(out, cap, raw);
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


static int emit_str(char *buf, size_t cap, const char *name, const char *val)
{
    if (!val || !val[0]) return 0;
    char q[208];
    nut_quote(q, sizeof(q), val);
    int n = snprintf(buf, cap, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, name, q);
    /* Never past the end: the caller adds this to its offset. */
    return (n < 0) ? 0 : (n >= (int)cap) ? (int)(cap ? cap - 1 : 0) : n;
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

/* The value NUT would send for `var`, unquoted, from the same sources as
 * LIST VAR: the fixed set first, then NUT's table. `all` is scratch of
 * all_cap bytes. False if this UPS has no such variable. */
static bool var_value(const ups_data_t *d, const char *var, char *all, size_t all_cap,
                      char *out, size_t cap)
{
    build_vars(d, all, all_cap);
    char want[96];
    snprintf(want, sizeof(want), "VAR %s %s \"", NUT_UPS_NAME, var);
    const char *hit = nut_find_line(all, want);
    if (!hit) return ups_hid_var_find(var, out, cap);
    hit += strlen(want);
    const char *end = strstr(hit, "\"\n");
    size_t n = end ? (size_t)(end - hit) : strlen(hit);
    if (n >= cap) n = cap - 1;
    memcpy(out, hit, n);
    out[n] = '\0';
    return true;
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
    bool dead;              /* a send failed or timed out: end the connection */
    bool seen_cmd;          /* one complete command so far: NUT_IDLE_S applies */
    uint32_t peer;          /* its address, for the per-peer count */
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
    nut_linebuf_t *lb;      /* the part-line carried between reads */
#if CONFIG_UPSA_TLS
    esp_tls_t *tls;         /* after STARTTLS; NULL in the clear */
    bool upgrade;           /* STARTTLS accepted: hand over to a TLS task */
#endif
} nut_conn_t;
#define NUT_BUF 1400

static void set_rcv_timeout(int sock, int secs)
{
    struct timeval tv = { .tv_sec = secs };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Every reply goes out through these, in the clear or, after STARTTLS, over
 * TLS. */
static void sendn(nut_conn_t *c, const char *s, size_t n)
{
    if (c->dead) return;
    while (n) {
#if CONFIG_UPSA_TLS
        ssize_t w = c->tls ? esp_tls_conn_write(c->tls, s, n) : send(c->sock, s, n, 0);
#else
        ssize_t w = send(c->sock, s, n, 0);
#endif
        if (w <= 0) { c->dead = true; return; }   /* error, or SO_SNDTIMEO */
        s += w;
        n -= (size_t)w;
    }
}

static void sendall(nut_conn_t *c, const char *s) { sendn(c, s, strlen(s)); }

static int conn_recv(nut_conn_t *c, char *rx, size_t cap)
{
#if CONFIG_UPSA_TLS
    if (c->tls) return (int)esp_tls_conn_read(c->tls, rx, cap);
#endif
    return recv(c->sock, rx, cap, 0);
}

/* A client's name, echoed back as one word: as it came if it is plain, else
 * quoted and escaped, so a name with a space or a quote in it cannot break
 * the reply's line. Written to out (cap bytes); returns out. */
static const char *echo_word(char *out, size_t cap, const char *name)
{
    if (!name[0] || strpbrk(name, " \t\"\\")) {
        out[0] = '"';
        nut_quote(out + 1, cap - 2, name);
        size_t l = strlen(out);
        out[l] = '"';
        out[l + 1] = '\0';
        return out;
    }
    return name;
}

/* INSTCMD and SET VAR go through here. Without a login on the board they are
 * refused; with one, the connection's USERNAME and PASSWORD must match it. */
static bool nut_may_write(nut_conn_t *c)
{
    switch (auth_nut_write(auth_src_of_fd(c->sock), c->have_user ? c->user : NULL, c->have_pass ? c->pass : NULL)) {
    case AUTHC_NUT_ALLOW:             return true;
    case AUTHC_NUT_USERNAME_REQUIRED: sendall(c, "ERR USERNAME-REQUIRED\n"); break;
    case AUTHC_NUT_PASSWORD_REQUIRED: sendall(c, "ERR PASSWORD-REQUIRED\n"); break;
    default:
        sendall(c, "ERR ACCESS-DENIED\n");
        /* One wrong password per connection: with a login set, the connection
         * ends, so a client cannot try again on it without a new connection
         * (and the per-source count that goes with one). */
        if (auth_enabled()) c->dead = true;
        break;
    }
    return false;
}

/* Returns false when the connection should close (LOGOUT). */
static bool handle_line(nut_conn_t *c, char *line)
{
    /* (A reply that shows the server working counts toward the OTA confirm
     * gate as a page view does -- see the LIST VAR and GET VAR replies. Any
     * line at all used to count, even "ERR UNKNOWN-COMMAND".) */
    char *a[5];
    int argc = split(line, a, 5);
    if (argc == 0) return true;

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
                 "My UPS Pal %s - NUT compatible - http://networkupstools.org/\n",
                 app->version);
        sendall(c, buf);

    } else if (!strcmp(a[0], "NETVER") || !strcmp(a[0], "PROTVER")) {
        sendall(c, "1.3\n");

    } else if (!strcmp(a[0], "HELP")) {
        sendall(c, "Commands: HELP VER NETVER LIST GET SET INSTCMD "
                      "LOGIN LOGOUT USERNAME PASSWORD"
#if CONFIG_UPSA_TLS
                      " STARTTLS"
#endif
                      "\n");

    } else if (!strcmp(a[0], "STARTTLS")) {
#if CONFIG_UPSA_TLS
        /* As upsd: OK STARTTLS, then the TLS handshake on the same socket,
         * with the board's own certificate (components/tlscert). The rest
         * of the connection runs on a task with the stack a handshake
         * needs; connections that never ask for TLS keep the small one. */
        const uint8_t *crt, *key;
        size_t crt_len, key_len;
        if (c->tls) {
            sendall(c, "ERR ALREADY-SSL-MODE\n");
        } else if (!tlscert_get(&crt, &crt_len, &key, &key_len)) {
            sendall(c, "ERR FEATURE-NOT-CONFIGURED\n");
        } else {
            sendall(c, "OK STARTTLS\n");
            c->upgrade = true;
            return false;
        }
#else
        /* upsd's answer when it has no certificate: clients carry on in
         * the clear, as they would with real upsd. */
        sendall(c, "ERR FEATURE-NOT-CONFIGURED\n");
#endif

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
            sendall(c, "ERR INVALID-ARGUMENT\n");
        } else if (*have) {
            sendall(c, is_user ? "ERR ALREADY-SET-USERNAME\n" : "ERR ALREADY-SET-PASSWORD\n");
        } else {
            strncpy(dst, a[1], cap - 1);
            dst[cap - 1] = '\0';
            *have = true;
            sendall(c, "OK\n");
        }

    } else if (!strcmp(a[0], "LOGIN")) {
        sendall(c, argc >= 2 && !strcmp(a[1], NUT_UPS_NAME)
                      ? "OK\n" : "ERR UNKNOWN-UPS\n");

    } else if (!strcmp(a[0], "PRIMARY") || !strcmp(a[0], "MASTER")) {
        /* upsmon asks for this on every connect when it is the primary. It was
         * refused as an unknown command, which upsmon logs as an alert and
         * answers by skipping that poll. Nothing here is granted that a read
         * could not already do; INSTCMD still asks for the login. */
        if (argc < 2 || strcmp(a[1], NUT_UPS_NAME)) sendall(c, "ERR UNKNOWN-UPS\n");
        else sendall(c, a[0][0] == 'P' ? "OK PRIMARY-GRANTED\n" : "OK MASTER-GRANTED\n");

    } else if (!strcmp(a[0], "LOGOUT")) {
        /* upsd closes the connection after this, and clients expect it to. */
        sendall(c, "OK Goodbye\n");
        return false;

    } else if (!strcmp(a[0], "LIST") && argc >= 2 && !strcmp(a[1], "UPS")) {
        char desc[160];
        ups_desc(&d, desc, sizeof(desc));
        snprintf(buf, NUT_BUF, "BEGIN LIST UPS\nUPS %s \"%s\"\nEND LIST UPS\n",
                 NUT_UPS_NAME, desc);
        sendall(c, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        if (stale(&d)) { sendall(c, "ERR DATA-STALE\n"); return true; }
        snprintf(buf, NUT_BUF, "BEGIN LIST VAR %s\n", NUT_UPS_NAME);
        sendall(c, buf);
        build_vars(&d, buf, NUT_BUF);
        sendall(c, buf);
        /* Then everything NUT's own table for this UPS found (ups_vartab.h),
         * a line at a time so the list costs no stack however long it is.
         * The fixed set above wins where both have a name. */
        size_t nv = ups_hid_var_count();
        for (size_t i = 0; i < nv; i++) {
            const char *vn;
            char vv[40], q[80], line[160], key[80];
            if (!ups_hid_var_get(i, &vn, vv, sizeof(vv))) continue;
            snprintf(key, sizeof(key), "VAR %s %s ", NUT_UPS_NAME, vn);
            if (nut_find_line(buf, key)) continue;
            nut_quote(q, sizeof(q), vv);
            snprintf(line, sizeof(line), "VAR %s %s \"%s\"\n", NUT_UPS_NAME, vn, q);
            sendall(c, line);
        }
        snprintf(buf, NUT_BUF, "END LIST VAR %s\n", NUT_UPS_NAME);
        sendall(c, buf);
        if (!c->dead) ota_note_request_served();

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CMD")) {
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        snprintf(buf, NUT_BUF, "BEGIN LIST CMD %s\n", NUT_UPS_NAME);
        sendall(c, buf);
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!(d.caps & NUT_CMDS[i].cap)) continue;   /* only what it can do */
            snprintf(buf, NUT_BUF, "CMD %s %s\n", NUT_UPS_NAME, NUT_CMDS[i].name);
            sendall(c, buf);
        }
        snprintf(buf, NUT_BUF, "END LIST CMD %s\n", NUT_UPS_NAME);
        sendall(c, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "RW")) {
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        snprintf(buf, NUT_BUF, "BEGIN LIST RW %s\n", NUT_UPS_NAME);
        sendall(c, buf);
        if ((d.caps & UPS_CAP_LOWBATT) && ups_valid(d.lowbatt_limit)) {
            snprintf(buf, NUT_BUF, "RW %s battery.charge.low \"%.0f\"\n",
                     NUT_UPS_NAME, d.lowbatt_limit);
            sendall(c, buf);
        }
        snprintf(buf, NUT_BUF, "END LIST RW %s\n", NUT_UPS_NAME);
        sendall(c, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 3 && !strcmp(a[1], "CLIENT")) {
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        snprintf(buf, NUT_BUF, "BEGIN LIST CLIENT %s\nEND LIST CLIENT %s\n",
                 NUT_UPS_NAME, NUT_UPS_NAME);
        sendall(c, buf);

    } else if (!strcmp(a[0], "GET") && argc == 3 && !strcmp(a[1], "VAR")) {
        sendall(c, "ERR INVALID-ARGUMENT\n");     /* "GET VAR ups" names no variable */

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "VAR")) {
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        if (stale(&d)) { sendall(c, "ERR DATA-STALE\n"); return true; }
        /* Built from the same function as LIST VAR, so a variable can never
         * appear in one and not the other. */
        char *all = c->all;
        build_vars(&d, all, NUT_BUF);
        char want[80];
        int wn = snprintf(want, sizeof(want), "VAR %s %s ", NUT_UPS_NAME, a[3]);
        char *hit = (char *)nut_find_line(all, want);
        if (!hit) {
            char vv[40], q[80];
            if (ups_hid_var_find(a[3], vv, sizeof(vv))) {
                nut_quote(q, sizeof(q), vv);
                snprintf(buf, NUT_BUF, "VAR %s %s \"%s\"\n", NUT_UPS_NAME, a[3], q);
                sendall(c, buf);
                if (!c->dead) ota_note_request_served();
            } else {
                sendall(c, "ERR VAR-NOT-SUPPORTED\n");
            }
            return true;
        }
        char *end = strchr(hit, '\n');
        size_t len = end ? (size_t)(end - hit + 1) : strlen(hit);
        (void)wn;
        sendn(c, hit, len);
        if (!c->dead) ota_note_request_served();

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "TYPE")) {
        /* PeaNUT asks this of every variable LIST VAR gives it, on the same
         * connection, and takes any ERR as the UPS being unreachable: the
         * whole UPS showed "Device Unreachable" until this was answered.
         * As upsd does (netget.c): RW for a writable one, then STRING:n for
         * text, NUMBER otherwise. */
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        if (stale(&d)) { sendall(c, "ERR DATA-STALE\n"); return true; }
        char v[64];
        if (!var_value(&d, a[3], c->all, NUT_BUF, v, sizeof(v))) {
            sendall(c, "ERR VAR-NOT-SUPPORTED\n");
            return true;
        }
        char *end;
        (void)strtod(v, &end);
        bool num = v[0] && *end == '\0';
        bool rw = !strcmp(a[3], "battery.charge.low") && (d.caps & UPS_CAP_LOWBATT);
        snprintf(buf, NUT_BUF, "TYPE %s %s%s %s\n", NUT_UPS_NAME, a[3],
                 rw ? " RW" : "", num ? "NUMBER" : "STRING:64");
        sendall(c, buf);

    } else if (!strcmp(a[0], "LIST") && argc >= 4 &&
               (!strcmp(a[1], "ENUM") || !strcmp(a[1], "RANGE"))) {
        /* No variable here has a list of allowed values or a range: an empty
         * list, as upsd gives for one without, not an error. */
        if (!named_ok) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        const char *nm = echo_word(c->all, NUT_BUF, a[3]);
        snprintf(buf, NUT_BUF, "BEGIN LIST %s %s %s\nEND LIST %s %s %s\n",
                 a[1], NUT_UPS_NAME, nm, a[1], NUT_UPS_NAME, nm);
        sendall(c, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "DESC")) {
        const char *desc = "Description unavailable";
        for (size_t i = 0; i < sizeof(NUT_DESCS) / sizeof(NUT_DESCS[0]); i++) {
            if (!strcmp(NUT_DESCS[i].name, a[3])) { desc = NUT_DESCS[i].desc; break; }
        }
        snprintf(buf, NUT_BUF, "DESC %s %s \"%s\"\n", NUT_UPS_NAME,
                 echo_word(c->all, NUT_BUF, a[3]), desc);
        sendall(c, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "UPSDESC")) {
        char desc[160];
        ups_desc(&d, desc, sizeof(desc));
        snprintf(buf, NUT_BUF, "UPSDESC %s \"%s\"\n", NUT_UPS_NAME, desc);
        sendall(c, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 3 && !strcmp(a[1], "NUMLOGINS")) {
        snprintf(buf, NUT_BUF, "NUMLOGINS %s 1\n", NUT_UPS_NAME);
        sendall(c, buf);

    } else if (!strcmp(a[0], "GET") && argc >= 4 && !strcmp(a[1], "CMDDESC")) {
        const char *nm = echo_word(c->all, NUT_BUF, a[3]);
        char q[80];
        nut_quote(q, sizeof(q), a[3]);
        snprintf(buf, NUT_BUF, "CMDDESC %s %s \"%s\"\n", NUT_UPS_NAME, nm, q);
        sendall(c, buf);

    } else if (!strcmp(a[0], "INSTCMD") && argc >= 3) {
        const nut_cmd_t *cmd = NULL;
        for (size_t i = 0; i < sizeof(NUT_CMDS) / sizeof(NUT_CMDS[0]); i++) {
            if (!strcmp(NUT_CMDS[i].name, a[2])) { cmd = &NUT_CMDS[i]; break; }
        }
        if (strcmp(a[1], NUT_UPS_NAME)) sendall(c, "ERR UNKNOWN-UPS\n");
        else if (!cmd)                  sendall(c, "ERR CMD-NOT-SUPPORTED\n");
        else if (!nut_may_write(c))     { /* refusal already sent */ }
        else {
            /* No INSTCMD here takes an argument. load.off carries an optional
             * delay in NUT; this passes 0, meaning immediately, which is what
             * `upscmd ups load.off` with no argument asks for. (This used to
             * read `c->cmd == UPS_CMD_LOAD_OFF ? 0 : 0` -- a ternary with the
             * same value in both branches, which looked like it handled the
             * delay and did nothing at all.) */
            esp_err_t err = ups_hid_command(cmd->cmd, 0);
            sendall(c, err == ESP_OK ? "OK\n"
                        : err == ESP_ERR_NOT_SUPPORTED ? "ERR CMD-NOT-SUPPORTED\n"
                        : "ERR INSTCMD-FAILED\n");
        }

    } else if (!strcmp(a[0], "SET") && argc >= 5 && !strcmp(a[1], "VAR")) {
        if (strcmp(a[2], NUT_UPS_NAME)) { sendall(c, "ERR UNKNOWN-UPS\n"); return true; }
        if (strcmp(a[3], "battery.charge.low")) {
            sendall(c, "ERR VAR-NOT-SUPPORTED\n");
            return true;
        }
        if (!nut_may_write(c)) return true;
        esp_err_t err = ups_hid_command(UPS_CMD_LOWBATT_LIMIT, atoi(a[4]));
        sendall(c, err == ESP_OK              ? "OK\n"
                    : err == ESP_ERR_INVALID_ARG ? "ERR INVALID-ARGUMENT\n"
                                                 : "ERR SET-FAILED\n");

    } else {
        sendall(c, "ERR UNKNOWN-COMMAND\n");
    }
    return true;
}

/* One complete line from nut_line_feed(). */
static bool on_line(void *ctx, char *line, bool overlong)
{
    nut_conn_t *c = ctx;
    if (overlong) {
        sendall(c, "ERR INVALID-ARGUMENT\n");
        return true;
    }
    /* The first line, of any kind, ends the short first-command deadline. */
    if (!c->seen_cmd) {
        c->seen_cmd = true;
        set_rcv_timeout(c->sock, nut_conn_timeout_s(true, NUT_FIRST_CMD_S, NUT_IDLE_S));
    }
    ESP_LOGD(TAG, "<- %s", strncasecmp(line, "PASSWORD", 8) ? line : "PASSWORD (hidden)");
    return handle_line(c, line);
}

/* Give back this address's slot in the per-peer table. */
static void peer_release(uint32_t peer)
{
    for (size_t i = 0; peer && i < sizeof(s_peer_addr) / sizeof(s_peer_addr[0]); i++) {
        uint32_t want = peer;
        if (atomic_compare_exchange_strong(&s_peer_addr[i], &want, 0)) return;
    }
}

/* The connection's end, whichever task it is on. */
static void conn_end(nut_conn_t *c)
{
#if CONFIG_UPSA_TLS
    if (c->tls) esp_tls_server_session_delete(c->tls);
#endif
    if (c->sock >= 0) close(c->sock);
    free(c->buf);
    free(c->all);
    if (c->lb) memset(c->lb, 0, sizeof(*c->lb));    /* may hold a PASSWORD line */
    free(c->lb);
    peer_release(c->peer);
    memset(c, 0, sizeof(*c));                       /* and the password itself */
    free(c);
    s_clients--;
    /* Headroom left on this task's stack at its deepest, in bytes: a real
     * number to size it by, after it overflowed once. */
    ESP_LOGI(TAG, "client disconnected (%d left, stack headroom %u)", (int)s_clients,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* Read and answer lines until the client goes. */
static void serve(nut_conn_t *c)
{
    char rx[128];
    for (;;) {
        int n = conn_recv(c, rx, sizeof(rx));
        if (n <= 0) break;                  /* closed, reset, or idle too long */
        if (!nut_line_feed(c->lb, rx, (size_t)n, on_line, c)) break;
        if (c->dead) break;                 /* the client stopped reading */
    }
    memset(rx, 0, sizeof(rx));
}

#if CONFIG_UPSA_TLS
/* A client that says STARTTLS and then never starts the handshake. */
#define NUT_HANDSHAKE_S 10

/* The TLS part of a connection, on whichever task has it. Ends the
 * connection. */
static void tls_run(nut_conn_t *c)
{
    const uint8_t *crt, *key;
    size_t crt_len, key_len;
    struct timeval hs = { .tv_sec = NUT_HANDSHAKE_S };
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &hs, sizeof(hs));
    c->tls = tlscert_get(&crt, &crt_len, &key, &key_len) ? esp_tls_init() : NULL;
    if (c->tls) {
        esp_tls_cfg_server_t cfg = {
            .servercert_buf = crt, .servercert_bytes = crt_len,
            .serverkey_buf = key,  .serverkey_bytes = key_len,
            .tls_handshake_timeout_ms = NUT_HANDSHAKE_S * 1000,
        };
        if (esp_tls_server_session_create(&cfg, c->sock, c->tls) != 0) {
            ESP_LOGW(TAG, "STARTTLS handshake failed");
            esp_tls_server_session_delete(c->tls);
            c->tls = NULL;
            conn_end(c);
            return;
        }
        /* Over TLS it has sent no command yet, whatever it sent in the clear. */
        c->seen_cmd = false;
        set_rcv_timeout(c->sock, nut_conn_timeout_s(false, NUT_FIRST_CMD_S, NUT_IDLE_S));
        ESP_LOGI(TAG, "client is on TLS");
        serve(c);
    }
    conn_end(c);
}

#endif

/* One client, start to finish, on whichever task has it. */
static void client_run(const nut_accepted_t *acc)
{
    int sock = acc->sock;
    nut_conn_t *c = conn_calloc(1, sizeof(*c));
    if (c) {
        c->sock = sock;
        c->peer = acc->peer;
        c->buf = conn_calloc(1, NUT_BUF);
        c->all = conn_calloc(1, NUT_BUF);
        /* On the heap with the reply buffers, not on this task's stack,
         * which has overflowed before. */
        c->lb = conn_calloc(1, sizeof(*c->lb));
    }
    if (!c || !c->buf || !c->all || !c->lb) {
        ESP_LOGE(TAG, "no memory for a client; closing it");
        if (c) conn_end(c);
        else { peer_release(acc->peer); s_clients--; close(sock); }
        return;
    }

    serve(c);
#if CONFIG_UPSA_TLS
    if (c->upgrade) {
        c->upgrade = false;
        /* Anything after STARTTLS in the same read came in the clear and is
         * not part of the TLS stream. */
        memset(c->lb, 0, sizeof(*c->lb));
        tls_run(c);                     /* a worker's stack has room for it */
        return;
    }
#endif
    conn_end(c);
}

/* Never returns and is never deleted (see NUT_WORKERS). */
static void nut_worker_task(void *arg)
{
    for (;;) {
        nut_accepted_t acc;
        if (xQueueReceive(s_accepted, &acc, portMAX_DELAY) == pdTRUE) client_run(&acc);
    }
}

/* Make one more worker. Only nut_listen calls it. */
static bool add_worker(void)
{
    char name[] = "nut_w0";
    name[5] = (char)('0' + s_workers);
#if CONFIG_SPIRAM
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(nut_worker_task, name, NUT_WORKER_STACK, NULL,
                                                   4, NULL, tskNO_AFFINITY, NUT_STACK_CAPS);
#else
    BaseType_t r = xTaskCreate(nut_worker_task, name, NUT_WORKER_STACK, NULL, 4, NULL);
#endif
    if (r != pdPASS) return false;
    s_workers++;
    return true;
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

        /* The setup/rescue AP is open to anyone in radio range. Nothing about
         * the UPS is served there. */
        struct sockaddr_storage here;
        socklen_t hlen = sizeof(here);
        if (getsockname(sock, (struct sockaddr *)&here, &hlen) == 0 &&
            netmgr_local_is_ap(netmgr_sockaddr_v4(&here, hlen))) {
            close(sock);
            continue;
        }

        /* Clients hold the connection open indefinitely; keepalive is what
         * reclaims sockets when a NAS is yanked rather than disconnected, and
         * only if it is told to look sooner than lwIP's two hours. */
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        int ka_idle = NUT_KEEPIDLE_S, ka_intvl = NUT_KEEPINTVL_S, ka_cnt = NUT_KEEPCNT;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof(ka_cnt));
        /* Short until the first command; on_line() raises it then. */
        set_rcv_timeout(sock, nut_conn_timeout_s(false, NUT_FIRST_CMD_S, NUT_IDLE_S));
        struct timeval snd = { .tv_sec = NUT_SEND_S };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

        uint32_t peer_ip = peer.sin_addr.s_addr;
        int same = 0;
        for (size_t i = 0; i < sizeof(s_peer_addr) / sizeof(s_peer_addr[0]); i++)
            if (s_peer_addr[i] == peer_ip) same++;
        if (same >= NUT_PER_PEER) {
            ESP_LOGW(TAG, "%s already has %d connections, refusing another",
                     inet_ntoa(peer.sin_addr), NUT_PER_PEER);
            close(sock);
            continue;
        }
        if (s_clients >= NUT_WORKERS) {
            ESP_LOGW(TAG, "%d clients already, refusing %s", NUT_WORKERS,
                     inet_ntoa(peer.sin_addr));
            close(sock);
            continue;
        }
        /* Every worker busy: make the next one. s_clients only falls
         * meanwhile (a worker finishing), so at worst one is made a moment
         * early; it is kept either way. */
        if (s_clients >= s_workers && !add_worker()) {
            ESP_LOGW(TAG, "no memory for a NUT worker, refusing %s",
                     inet_ntoa(peer.sin_addr));
            close(sock);
            continue;
        }
        ESP_LOGI(TAG, "client from %s", inet_ntoa(peer.sin_addr));
        s_clients++;
        for (size_t i = 0; i < sizeof(s_peer_addr) / sizeof(s_peer_addr[0]); i++) {
            uint32_t zero = 0;
            if (atomic_compare_exchange_strong(&s_peer_addr[i], &zero, peer_ip)) break;
        }
        nut_accepted_t acc = { .sock = sock, .peer = peer_ip };
        /* Never blocks: at most NUT_WORKERS sockets are connected, queued or
         * served, and the queue holds that many. */
        xQueueSend(s_accepted, &acc, portMAX_DELAY);
    }
}

int nut_server_clients(void) { return (int)s_clients; }

const char *nut_server_ups_name(void) { return NUT_UPS_NAME; }

esp_err_t nut_server_start(uint16_t port)
{
    /* Workers are made as clients arrive (nut_listen), not here. */
    s_accepted = xQueueCreate(NUT_WORKERS, sizeof(nut_accepted_t));
    if (!s_accepted) return ESP_ERR_NO_MEM;
    /* 3 KB: accept, setsockopt, a log line with inet_ntoa, and making a
     * worker. No client is served on it. */
    if (xTaskCreate(nut_listen_task, "nut_listen", 3072,
                    (void *)(uintptr_t)port, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
