#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CPL rotation audit -- flag every placement JLC could fit the wrong way round.

WHAT THIS IS AND IS NOT

JLCPCB place a part using THEIR library footprint for the LCSC code you ordered,
rotated by the CPL value. So the CPL is only correct if KiCad's footprint
orientation and JLC's agree at 0 degrees. For whole package families they do
not, and SOT-23 is the notorious one.

The christmas-tree-sensor repo resolves this properly, by fetching JLC's own
footprint for each LCSC code and computing the delta. That needs LCSC codes,
which this board does not have yet. So this script does the part that is
possible now: it enumerates every placement whose orientation could be
misinterpreted and says which ones a human must confirm against JLC's assembly
preview before ordering.

A pass here does NOT mean the CPL is right. It means you have been handed the
list of things to check. Nothing else in the toolchain will catch a rotated
part: it is not a DRC error, not a netlist error, and not visible in a render.
"""
import collections
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CPL = os.path.join(ROOT, "hardware", "fab", "ups-adaptor-cpl.csv")

# Package families where KiCad and JLC are known or likely to disagree.
# SOT-23 is called out explicitly by the source repo's audit as unresolved
# between KiCad, the community rotation database and JLC's own preview.
RISKY = {
    "SOT-23": "the known offender; KiCad, the community DB and JLC disagree",
    "SOT-223": "tab orientation varies between libraries",
    "SOIC": "pin 1 corner convention varies",
    "USB": "connector bodies are frequently rotated in vendor libraries",
    "PinHeader": "pin 1 end must match, and these are hand-soldered anyway",
}


def main():
    if not os.path.exists(CPL):
        print("  SKIP no CPL -- run: kicad-cli pcb export pos")
        return 0

    rows = list(csv.DictReader(open(CPL)))
    print("  %d placements, %s" % (
        len(rows),
        ", ".join("%d %s" % (n, s) for s, n in
                  sorted(collections.Counter(r["Side"] for r in rows).items()))))

    flagged, rotated = [], []
    for r in rows:
        pkg = r.get("Package", "")
        rot = float(r.get("Rot", 0) or 0)
        why = next((v for k, v in RISKY.items() if k in pkg), None)
        if why:
            flagged.append((r["Ref"], pkg, rot, r["Side"], why))
        if abs(rot) > 0.01:
            rotated.append((r["Ref"], pkg, rot, r["Side"]))

    print("\n  %d placements are rotated; a wrong library delta shows up here first:"
          % len(rotated))
    for ref, pkg, rot, side in sorted(rotated)[:14]:
        print("     %-5s %-34s %6.0f deg  %s" % (ref, pkg[:34], rot, side))
    if len(rotated) > 14:
        print("     ... and %d more" % (len(rotated) - 14))

    print("\n  %d placements are in a package family known to disagree:" % len(flagged))
    seen = set()
    for ref, pkg, rot, side, why in sorted(flagged):
        fam = next(k for k in RISKY if k in pkg)
        if fam not in seen:
            seen.add(fam)
            print("     %-12s %s" % (fam, why))
        print("        %-5s %-30s %6.0f deg  %s" % (ref, pkg[:30], rot, side))

    print("\n  ACTION before ordering: upload these gerbers and this CPL to JLC and")
    print("  step through the assembly preview part by part. That preview is the")
    print("  only authority here -- this list just tells you where to look.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
