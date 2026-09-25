#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The firmware's own parser, compiled for the host, against a real descriptor.

Asserts the values the design depends on rather than just that it runs: the
per-report-type bit cursors, the unit-aware scaling, and collection
disambiguation are each checked against a field whose correct answer is known.
"""
import os, re, subprocess, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def main():
    caps = sorted(glob.glob(os.path.join(ROOT, "captures", "051d-0002-2*")))
    if not caps:
        print("  SKIP no capture"); return 0
    r = subprocess.run([os.path.join(ROOT, "tools", "host-test", "run.sh"), caps[0]],
                       capture_output=True, text=True)
    out = r.stdout + "\n" + r.stderr
    if r.returncode != 0:
        print("  FAIL host-test build/run failed\n" + r.stderr[-800:]); return 1

    fails = 0
    def want(pattern, why):
        nonlocal fails
        if re.search(pattern, out, re.M):
            print("  ok   %s" % why)
        else:
            print("  FAIL %s" % why); fails += 1

    want(r"->\s*86 fields", "86 power/battery fields parsed")
    want(r"^20 distinct NUT variables", "20 NUT variables published")
    want(r"^\s*12 Input\s+0\s+8 .*battery\.charge", "report 12 Input at bit 0")
    want(r"^\s*12 Feature\s+0\s+8 .*battery\.charge",
         "report 12 Feature also at bit 0 (per-type bit cursors)")
    want(r"^\s*12 \w+\s+8\s+16 .*battery\.runtime", "runtime at bit 8, 16 bits wide")
    want(r"^\s*49 Feature .*Input .*input\.voltage", "voltage in Input collection -> input.voltage")
    want(r"^\s*38 Feature .*Battery .*battery\.voltage", "voltage in Battery collection -> battery.voltage")
    want(r"^\s*49 Feature\s+0\s+16 Power\s+Input\s+7\s+0", "report 49 exponent 7 scales to shift 0 (raw volts)")
    want(r"^\s*9 Feature\s+0\s+16 Power\s+PowerSummary\s+5\s+-2", "report 9 exponent 5 scales to shift -2 (centivolts)")
    want(r'profile "generic"|generic', "APC resolves to the generic path, no vendor quirk")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
