/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "metrics.h"

#include <stdio.h>
#include <string.h>

#define NUT "network_ups_tools_"

/* Label values escape backslash, double quote and newline; nothing else. */
static const char *esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 2 < cap; in++) {
        if (*in == '\\' || *in == '"') { out[o++] = '\\'; out[o++] = *in; }
        else if (*in == '\n')          { out[o++] = '\\'; out[o++] = 'n'; }
        else                           { out[o++] = *in; }
    }
    out[o] = '\0';
    return out;
}

static void help(metrics_write_fn w, void *ctx, const char *name,
                 const char *type, const char *text)
{
    char line[256];
    snprintf(line, sizeof(line), "# HELP %s %s\n# TYPE %s %s\n", name, text, name, type);
    w(ctx, line);
}

/* One UPS gauge, named as nut_exporter names the NUT variable. Absent values
 * are skipped entirely: no HELP, no sample. */
static void ups_gauge(metrics_write_fn w, void *ctx, const char *ups,
                      const char *var, const char *text, float v)
{
    if (v == UPS_VALUE_ABSENT) return;
    char name[96], line[192];
    snprintf(name, sizeof(name), NUT "%s", var);
    for (char *p = name; *p; p++) if (*p == '.') *p = '_';
    help(w, ctx, name, "gauge", text);
    snprintf(line, sizeof(line), "%s{ups=\"%s\"} %g\n", name, ups, (double)v);
    w(ctx, line);
}

void metrics_render(const metrics_src_t *m, metrics_write_fn w, void *ctx)
{
    char ups[64], a[96], b[96], c[96], line[512];
    esc(m->ups_name ? m->ups_name : "ups", ups, sizeof(ups));
    const ups_data_t *d = m->ups;
    bool attached = d && d->attached;

    /* ---- the UPS, in nut_exporter's vocabulary ---- */
    if (attached) {
        help(w, ctx, NUT "device_info", "gauge",
             "UPS identity, as its USB descriptors report it. Always 1.");
        char vid[8], pid[8];
        snprintf(vid, sizeof(vid), "%04x", d->vid);
        snprintf(pid, sizeof(pid), "%04x", d->pid);
        snprintf(line, sizeof(line),
                 NUT "device_info{ups=\"%s\",mfr=\"%s\",model=\"%s\",serial=\"%s\","
                 "vid=\"%s\",pid=\"%s\",type=\"ups\"} 1\n",
                 ups, esc(d->mfr, a, sizeof(a)), esc(d->model, b, sizeof(b)),
                 esc(d->serial, c, sizeof(c)), vid, pid);
        w(ctx, line);

        ups_gauge(w, ctx, ups, "battery.charge", "Battery charge, percent.", d->battery_charge);
        ups_gauge(w, ctx, ups, "battery.runtime", "Battery runtime remaining, seconds.", d->battery_runtime);
        ups_gauge(w, ctx, ups, "battery.voltage", "Battery voltage, volts.", d->battery_voltage);
        ups_gauge(w, ctx, ups, "battery.charge.low", "Charge at which the UPS reports low battery, percent.", d->lowbatt_limit);
        ups_gauge(w, ctx, ups, "input.voltage", "Input voltage, volts.", d->input_voltage);
        ups_gauge(w, ctx, ups, "input.frequency", "Input frequency, hertz.", d->input_frequency);
        ups_gauge(w, ctx, ups, "output.voltage", "Output voltage, volts.", d->output_voltage);
        ups_gauge(w, ctx, ups, "ups.load", "Load, percent of capacity.", d->ups_load);
        ups_gauge(w, ctx, ups, "ups.realpower", "Real power drawn, watts, where the UPS meters it.", d->ups_realpower);
        ups_gauge(w, ctx, ups, "ups.realpower.nominal", "Nameplate real power, watts.", d->ups_realpower_nom);
        ups_gauge(w, ctx, ups, "ups.power", "Apparent power, VA.", d->ups_apparentpower);

        /* Every flag every scrape, 0 or 1, as nut_exporter does: an alert on
         * flag="OB" needs the series to exist while it is 0. Left out only
         * when the status itself is unknown. */
        if (d->status != UPS_STATUS_UNKNOWN) {
            static const struct { uint32_t bit; const char *flag; } F[] = {
                { UPS_STATUS_ONLINE, "OL" },     { UPS_STATUS_ONBATT, "OB" },
                { UPS_STATUS_LOWBATT, "LB" },    { UPS_STATUS_REPLACEBATT, "RB" },
                { UPS_STATUS_CHARGING, "CHRG" }, { UPS_STATUS_DISCHARGE, "DISCHRG" },
                { UPS_STATUS_OVERLOAD, "OVER" },
            };
            help(w, ctx, NUT "ups_status", "gauge",
                 "UPS status flags, as NUT names them: 1 when set.");
            for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
                snprintf(line, sizeof(line), NUT "ups_status{ups=\"%s\",flag=\"%s\"} %d\n",
                         ups, F[i].flag, (d->status & F[i].bit) ? 1 : 0);
                w(ctx, line);
            }
        }
    }

    /* ---- the board itself ---- */
    help(w, ctx, "ups_esp32_ups_attached", "gauge",
         "1 while a UPS is attached and answering, 0 otherwise.");
    snprintf(line, sizeof(line), "ups_esp32_ups_attached{ups=\"%s\"} %d\n", ups, attached ? 1 : 0);
    w(ctx, line);

    help(w, ctx, "ups_esp32_build_info", "gauge", "Firmware and board. Always 1.");
    snprintf(line, sizeof(line), "ups_esp32_build_info{version=\"%s\",board=\"%s\",hostname=\"%s\"} 1\n",
             esc(m->version, a, sizeof(a)), esc(m->board, b, sizeof(b)), esc(m->hostname, c, sizeof(c)));
    w(ctx, line);

    help(w, ctx, "ups_esp32_uptime_seconds", "gauge", "Seconds since the board started.");
    snprintf(line, sizeof(line), "ups_esp32_uptime_seconds %llu\n", (unsigned long long)m->uptime_s);
    w(ctx, line);

    help(w, ctx, "ups_esp32_crashes_total", "counter",
         "Restarts caused by a panic, watchdog or brownout, since the settings were last cleared.");
    snprintf(line, sizeof(line), "ups_esp32_crashes_total %u\n", (unsigned)m->crashes);
    w(ctx, line);

    help(w, ctx, "ups_esp32_heap_free_bytes", "gauge", "Free internal heap, bytes.");
    snprintf(line, sizeof(line), "ups_esp32_heap_free_bytes %u\n", (unsigned)m->heap_free);
    w(ctx, line);

    help(w, ctx, "ups_esp32_heap_min_free_bytes", "gauge",
         "Lowest free internal heap since the board started, bytes.");
    snprintf(line, sizeof(line), "ups_esp32_heap_min_free_bytes %u\n", (unsigned)m->heap_min_free);
    w(ctx, line);

    if (m->wifi_rssi != 0) {
        help(w, ctx, "ups_esp32_wifi_rssi_dbm", "gauge", "Wi-Fi signal strength, dBm.");
        snprintf(line, sizeof(line), "ups_esp32_wifi_rssi_dbm %d\n", m->wifi_rssi);
        w(ctx, line);
    }

    help(w, ctx, "ups_esp32_nut_clients", "gauge", "NUT clients connected on TCP 3493.");
    snprintf(line, sizeof(line), "ups_esp32_nut_clients %d\n", m->nut_clients);
    w(ctx, line);
}
