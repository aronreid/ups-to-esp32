#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assert the auto-reset circuit behaves as the design claims.

The cross-coupled emitters are the whole trick, and the claim made repeatedly in
the docs is that DTR and RTS at the SAME level assert nothing -- which is what
stops opening a serial monitor from resetting the board. That is worth checking
rather than asserting.
"""
import re, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
LOW, HIGH = 0.4, 2.0   # logic thresholds against a 3.3V rail

def main():
    r = subprocess.run(["ngspice", "-b", os.path.join(HERE, "spice", "autoreset.cir")],
                       capture_output=True, text=True)
    rows = {}
    for line in r.stdout.splitlines():
        m = re.match(r"(\w+) ([\d.e+-]+) ([\d.e+-]+) ([\d.e+-]+) ([\d.e+-]+)$", line.strip())
        if m:
            rows[m.group(1)] = tuple(float(m.group(i)) for i in range(2, 6))
    if not rows:
        print("  ngspice produced no results:\n" + r.stdout + r.stderr)
        return 1

    fails = 0
    def check(state, want_en, want_io0, why):
        nonlocal fails
        if state not in rows:
            print("  FAIL %-10s missing from simulation" % state); fails += 1; return
        _, _, en, io0 = rows[state]
        ok_en = (en < LOW) if want_en == "low" else (en > HIGH)
        ok_io = (io0 < LOW) if want_io0 == "low" else (io0 > HIGH)
        tag = "ok  " if (ok_en and ok_io) else "FAIL"
        if not (ok_en and ok_io):
            fails += 1
        print("  %s %-10s EN=%.3f (want %s)  IO0=%.3f (want %s)  -- %s"
              % (tag, state, en, want_en, io0, want_io0, why))

    check("both_low",  "high", "high", "idle, nothing asserted")
    check("both_high", "high", "high", "serial monitor must NOT reset the board")
    check("dtr_only",  "low",  "high", "RESET asserted, BOOT untouched")
    check("rts_only",  "high", "low",  "BOOT asserted, RESET untouched")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
