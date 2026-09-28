#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Is the published schematic the one the board is built from?

docs/schematic.pdf and docs/img/schematic.svg are for people without KiCad,
and they are what the README links. tools/gen-schematic.py writes them on
every run, with docs/schematic.sha256 naming the .kicad_sch they were drawn
from. If the schematic has changed since -- regenerated without committing the
exports, or edited by hand in the GUI, which the generator would overwrite --
the PDF shows a design that no longer exists, and this fails.

Needs no KiCad: it only hashes a file.
"""
import hashlib, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")
REC = os.path.join(ROOT, "docs", "schematic.sha256")
OUT = [os.path.join(ROOT, "docs", "schematic.pdf"),
       os.path.join(ROOT, "docs", "img", "schematic.svg")]


def main():
    bad = 0
    for p in OUT:
        if os.path.getsize(p) if os.path.exists(p) else 0:
            print("  ok   %s present" % os.path.relpath(p, ROOT))
        else:
            print("  FAIL %s is missing -- run tools/gen-schematic.py" % os.path.relpath(p, ROOT))
            bad += 1
    have = hashlib.sha256(open(SCH, "rb").read()).hexdigest()
    want = open(REC).read().strip() if os.path.exists(REC) else ""
    if have == want:
        print("  ok   the exports were drawn from the current schematic")
    else:
        print("  FAIL the schematic has changed since docs/schematic.pdf was drawn.")
        print("       Regenerate with tools/gen-schematic.py -- do not edit the")
        print("       schematic in the GUI; the generator overwrites it.")
        bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
