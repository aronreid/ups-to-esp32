#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The order files must describe the board that was actually designed.

hardware/bom/bom.csv once sat unregenerated for days while the design moved
under it. When it was finally read it still specified the AP2161W -- the
active-LOW load switch, the one defect in the whole review that would have
stopped the device working -- along with the wrong module, the wrong USB-A
connector, a deleted header and a 500 mA fuse. Every other check was green,
because no other check looks at the files that get UPLOADED.

This one does:
  - every schematic part is in the BOM, with the schematic's value; no extras
  - every line the JLC matcher cannot resolve by value carries an LCSC code
  - every part in the BOM is in the CPL and the other way round
  - nothing is placed on the bottom: Economic PCBA is single-sided only
"""
import csv
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BOM = os.path.join(ROOT, "hardware", "bom", "bom.csv")
CPL = os.path.join(ROOT, "hardware", "bom", "cpl.csv")

spec = importlib.util.spec_from_file_location(
    "gen_bom", os.path.join(ROOT, "tools", "gen-bom.py"))
gen_bom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_bom)

# There is no such thing as a line the matcher can be trusted to resolve.
# JLC's cart resolved every 0402 resistor on this board to an
# 01005 -- it read the "0402" in KiCad's "R_0402_1005Metric" as METRIC, which is
# imperial 01005, a part a quarter the length of the pads -- and resolved four
# 0402 capacitor lines to nothing at all. A blank code is a silent substitution
# waiting to happen, so every line carries one.


def main():
    fails = 0
    if not os.path.exists(BOM):
        print("  FAIL %s does not exist -- run tools/gen-bom.py" % BOM)
        return 1

    want = {}
    for entry in gen_bom.design():
        ref, _l, _s, value = entry[:4]
        if ref not in gen_bom.THT:
            want[ref] = value

    have, codes = {}, {}
    for row in csv.DictReader(open(BOM)):
        for ref in row["Designator"].split(","):
            have[ref] = row["Comment"]
            codes[ref] = (row["LCSC Part #"], row["Footprint"])

    missing = sorted(set(want) - set(have))
    extra = sorted(set(have) - set(want))
    wrong = sorted(r for r in want if r in have and have[r] != want[r])
    if missing or extra or wrong:
        fails += 1
        print("  FAIL the BOM does not match the schematic:")
        for r in missing:
            print("         %s (%s) is in the schematic, not the BOM" % (r, want[r]))
        for r in extra:
            print("         %s (%s) is in the BOM, not the schematic" % (r, have[r]))
        for r in wrong:
            print("         %s is %s in the BOM but %s in the schematic"
                  % (r, have[r], want[r]))
    else:
        print("  ok   BOM matches the schematic: %d parts, values identical" % len(want))

    bare = sorted(r for r, (code, _fp) in codes.items() if not code)
    if bare:
        fails += 1
        print("  FAIL %d lines have no LCSC code, which hands the choice to JLC's"
              % len(bare))
        print("       matcher: %s"
              % ", ".join("%s (%s)" % (r, have[r]) for r in bare))
        print("       It resolved this board's 0402 resistors to 01005 parts the")
        print("       last time it was given the chance. Pin the code in gen-bom.py.")
    else:
        print("  ok   all %d parts carry a pinned LCSC code -- nothing is left to"
              % len(codes))
        print("       JLC's matcher, which substitutes silently inside the cart")

    # The 0402/01005 trap itself: an imperial 0402 pad with a metric-0402 part.
    # Both are spelled "0402" and only one of them fits.
    SIZED = {"C11702": "0402", "C25744": "0402", "C25900": "0402",
             "C25905": "0402", "C25741": "0402", "C1525": "0402",
             "C52923": "0402", "C45783": "0805", "C15008": "1206",
             "C2286": "0603", "C2287": "0603"}
    mismatched = []
    for ref, (code, fp) in sorted(codes.items()):
        want_size = SIZED.get(code)
        if want_size and "_%s_" % want_size not in fp:
            mismatched.append("%s: %s is %s, footprint is %s"
                              % (ref, code, want_size, fp))
    if mismatched:
        fails += 1
        print("  FAIL part size does not match the footprint:")
        for m in mismatched:
            print("         %s" % m)
    else:
        print("  ok   every pinned passive is the imperial size its footprint is")
        print("       drawn for (0402 imperial = 1005 metric, NOT metric 0402)")

    # Parts whose near-identical siblings would destroy the board.
    LETHAL = {
        "U3": ("C150716", "AP2114HA is the same SOT-223 with pin 1 VIN and pin 2 "
                          "GND -- fitting it shorts the input rail to ground"),
        "U4": ("C110466", "AP2161W is the same SOT-223-5 with an ACTIVE LOW "
                          "enable -- the UPS would never power up"),
    }
    for ref, (code, why) in LETHAL.items():
        got = codes.get(ref, ("", ""))[0]
        if got != code:
            print("  FAIL %s must be LCSC %s, the BOM says %r" % (ref, code, got))
            print("       %s" % why)
            fails += 1
        else:
            print("  ok   %s pinned to %s (a near-identical sibling would kill the "
                  "board)" % (ref, code))

    if not os.path.exists(CPL):
        print("  FAIL %s does not exist -- run tools/build-pcb.sh" % CPL)
        return 1
    placed, bottom = set(), []
    for row in csv.DictReader(open(CPL)):
        placed.add(row["Designator"])
        if row["Layer"].lower() != "top":
            bottom.append(row["Designator"])
    if placed != set(have):
        fails += 1
        print("  FAIL BOM and CPL disagree: only in BOM %s, only in CPL %s"
              % (sorted(set(have) - placed), sorted(placed - set(have))))
    else:
        print("  ok   CPL places exactly the %d parts the BOM lists" % len(placed))
    if bottom:
        fails += 1
        print("  FAIL %d parts are on the BOTTOM: %s" % (len(bottom), ", ".join(sorted(bottom))))
        print("       JLCPCB Economic PCBA is single-sided placement only. Double-sided")
        print("       needs Standard PCBA, whose minimum board is 70 x 70 mm.")
    else:
        print("  ok   every part is on the top side, as Economic PCBA requires")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
