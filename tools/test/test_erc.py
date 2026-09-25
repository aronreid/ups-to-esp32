#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ERC gate. One violation is known and accepted; any other is a regression.

Accepted: CH340C V3 and AMS1117 VO are both typed "power output". Tying V3 to
VCC is correct at 3.3V operation -- the symbol types it as an output because
that is its behaviour at 5V, where it would need a 100nF cap instead. A
symbol-typing artifact, not a design fault.
"""
import os
import sys, re, subprocess, sys, tempfile

# Locate KiCad without hardcoding one machine's install path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ""))
import kicad_paths

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KC = kicad_paths.cli()
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")
ACCEPTED = 1

def main():
    rpt = tempfile.mktemp(suffix=".rpt")
    subprocess.run([KC, "sch", "erc", "--severity-error", "-o", rpt, SCH],
                   capture_output=True, text=True)
    if not os.path.exists(rpt):
        print("  FAIL ERC did not run"); return 1
    text = open(rpt).read()
    m = re.search(r"ERC messages: (\d+)\s+Errors (\d+)", text)
    n = int(m.group(2)) if m else -1
    v3 = "V3, Power output" in text
    if n == ACCEPTED and v3:
        print("  ok   1 violation, the known CH340C V3 / AMS1117 VO typing artifact")
        return 0
    if n == 0:
        print("  ok   ERC clean (the V3 artifact is gone -- update ACCEPTED)")
        return 0
    print("  FAIL %d ERC errors, expected %d" % (n, ACCEPTED))
    for line in text.splitlines():
        if line.startswith("[") or line.strip().startswith("@("):
            print("       " + line.strip())
    return 1

if __name__ == "__main__":
    sys.exit(main())
