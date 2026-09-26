#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Is /metrics valid Prometheus exposition, named as nut_exporter names it?

firmware/components/webui/metrics.c renders the scrape target. It is compiled
on the host and fed three boards -- a UPS on mains, one on battery reporting
only some values, and none attached -- and every output is checked:

- parseable: every sample preceded by its TYPE, metric and label names legal,
  label values escaped, values numeric, no series twice;
- honest: a value the UPS does not report is ABSENT, never 0;
- compatible: nut_exporter's names (network_ups_tools_battery_charge,
  network_ups_tools_ups_status{flag=...}), and every status flag present as
  0 or 1, because an alert on flag="OB" needs the series while it is 0.

If prometheus_client is installed, its own parser must accept the output too.
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WEBUI = os.path.join(ROOT, "firmware", "components", "webui")
UPS_INC = os.path.join(ROOT, "firmware", "components", "ups_hid", "include")

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "metrics.h"
static void out(void *ctx, const char *t) { (void)ctx; fputs(t, stdout); }
int main(int argc, char **argv)
{
    int which = argc > 1 ? argv[1][0] - '0' : 0;
    ups_data_t d;
    memset(&d, 0, sizeof(d));
    d.battery_charge = d.battery_runtime = d.battery_voltage = d.input_voltage = UPS_VALUE_ABSENT;
    d.input_frequency = d.output_voltage = d.ups_load = d.ups_realpower = UPS_VALUE_ABSENT;
    d.ups_realpower_nom = d.ups_apparentpower = d.lowbatt_limit = UPS_VALUE_ABSENT;
    strcpy(d.mfr, "CPS"); strcpy(d.model, "EC850LCD \"test\"\\x"); strcpy(d.serial, "ABC123");
    d.vid = 0x0764; d.pid = 0x0501;
    if (which == 0) {           /* on mains, full set */
        d.attached = true; d.status = UPS_STATUS_ONLINE | UPS_STATUS_CHARGING;
        d.battery_charge = 100; d.battery_runtime = 2040; d.battery_voltage = 13.6f;
        d.input_voltage = 121.5f; d.output_voltage = 121; d.ups_load = 19;
        d.ups_realpower_nom = 510; d.lowbatt_limit = 10;
    } else if (which == 1) {    /* on battery, sparse: no voltages at all */
        d.attached = true; d.status = UPS_STATUS_ONBATT | UPS_STATUS_DISCHARGE;
        d.battery_charge = 64; d.ups_load = 23;
    }                           /* 2: nothing attached */
    metrics_src_t m = {
        .ups = &d, .ups_name = "ups", .board = "UPS to ESP32 Module Rev A",
        .version = "v0.08", .hostname = "ups-esp32-4925", .uptime_s = 3600,
        .heap_free = 216000, .heap_min_free = 145000,
        .wifi_rssi = which == 2 ? 0 : -52, .nut_clients = 2, .crashes = 0,
    };
    metrics_render(&m, out, NULL);
    return 0;
}
'''

NAME = r"[a-zA-Z_:][a-zA-Z0-9_:]*"
LABEL = r'[a-zA-Z_][a-zA-Z0-9_]*="(?:[^"\\\n]|\\["\\n])*"'
SAMPLE = re.compile(rf'^({NAME})(\{{{LABEL}(?:,{LABEL})*\}})? (-?[0-9.eE+-]+|NaN|\+Inf|-Inf)$')


def validate(text):
    """Returns (errors, {series: value})."""
    errs, typed, series = [], set(), {}
    for n, line in enumerate(text.splitlines(), 1):
        if line.startswith("# HELP "):
            continue
        if line.startswith("# TYPE "):
            parts = line.split()
            if len(parts) != 4 or parts[3] not in ("gauge", "counter"):
                errs.append(f"line {n}: bad TYPE: {line}")
            typed.add(parts[2])
            continue
        m = SAMPLE.match(line)
        if not m:
            errs.append(f"line {n}: not a valid sample: {line!r}")
            continue
        name, labels, val = m.group(1), m.group(2) or "", m.group(3)
        if name not in typed:
            errs.append(f"line {n}: {name} has no TYPE before its sample")
        key = name + labels
        if key in series:
            errs.append(f"line {n}: duplicate series {key}")
        series[key] = float(val)
    return errs, series


def main():
    fails = 0
    def check(label, cond, detail=""):
        nonlocal fails
        print(("  ok   " if cond else "  FAIL ") + label + ("" if cond else f"  {detail}"))
        fails += 0 if cond else 1

    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        open(c, "w").write(HARNESS)
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", os.path.join(WEBUI, "include"), "-I", UPS_INC,
                            "-o", exe, c, os.path.join(WEBUI, "metrics.c")],
                           capture_output=True, text=True)
        if r.returncode:
            print("  FAIL metrics.c does not compile on the host:\n" + r.stderr[:1500])
            return 1
        outs = [subprocess.run([exe, str(i)], capture_output=True, text=True).stdout for i in range(3)]

    try:
        from prometheus_client.parser import text_string_to_metric_families as official
    except ImportError:
        official = None

    for i, name in enumerate(("on mains", "on battery, sparse", "no UPS attached")):
        errs, s = validate(outs[i])
        check(f"{name}: valid exposition format", not errs, "; ".join(errs[:3]))
        if official:
            try:
                list(official(outs[i]))
                check(f"{name}: prometheus_client's own parser accepts it", True)
            except Exception as e:
                check(f"{name}: prometheus_client's own parser accepts it", False, str(e))

        if i == 0:
            check("battery charge under nut_exporter's name",
                  s.get('network_ups_tools_battery_charge{ups="ups"}') == 100)
            check("dots become underscores: ups.realpower.nominal",
                  s.get('network_ups_tools_ups_realpower_nominal{ups="ups"}') == 510)
            check("OL=1 and OB=0, both present",
                  s.get('network_ups_tools_ups_status{ups="ups",flag="OL"}') == 1
                  and s.get('network_ups_tools_ups_status{ups="ups",flag="OB"}') == 0)
            info = [k for k in s if k.startswith("network_ups_tools_device_info")]
            check('device_info escapes a quote and a backslash in the model',
                  len(info) == 1 and 'model="EC850LCD \\"test\\"\\\\x"' in info[0], info)
            check("board health present",
                  s.get("ups_esp32_heap_min_free_bytes") == 145000 and s.get("ups_esp32_wifi_rssi_dbm") == -52)
        if i == 1:
            check("OB=1 while on battery", s.get('network_ups_tools_ups_status{ups="ups",flag="OB"}') == 1)
            check("unreported voltages are ABSENT, not 0",
                  not any(k.startswith(("network_ups_tools_input_voltage", "network_ups_tools_battery_voltage",
                                        "network_ups_tools_output_voltage")) for k in s))
            check("reported values still there", s.get('network_ups_tools_battery_charge{ups="ups"}') == 64)
        if i == 2:
            check("no UPS: attached=0 and no UPS series at all",
                  s.get('ups_esp32_ups_attached{ups="ups"}') == 0
                  and not any(k.startswith("network_ups_tools_") for k in s))
            check("no Wi-Fi signal: rssi left out, not 0", "ups_esp32_wifi_rssi_dbm" not in s)

    if not official:
        print("  note prometheus_client not installed; the built-in format check ran alone")
    print(f"\n{'FAIL' if fails else 'PASS'}: Prometheus /metrics")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
