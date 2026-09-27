#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The CPL's angles are KiCad's own, except where a human has overridden one.

This test used to assert the opposite -- that gen-bom.py had applied the
JLCKicadTools "reel angle" table to eight parts. JLC's assembly preview showed that table putting Q2 ninety degrees out: the board's SOT-23
pads sit two-down and one-up, and the model's leads came out left and right.
The table is community-maintained and undated, and a correction nobody can
verify is not safer than no correction.

So the policy is now: emit KiCad's angle, and override a part only when JLC's
preview has actually been looked at and shows that part wrong. The preview
renders JLC's own library model at our angle on top of our gerbers, so "the
leads sit on the pads" is something a human can see and a reason they can write
down. This test enforces exactly that:

  - every CPL angle equals the KiCad angle, unless the part is in
    gen-bom.ROTATION_OVERRIDE
  - every override states its evidence, so a magic number cannot creep in
  - the vendored table is not being applied silently

    python3 tools/test/test_rotation.py
"""
import csv
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CPL = os.path.join(ROOT, "hardware", "bom", "cpl.csv")
KICAD_POS = os.path.join(ROOT, "hardware", "fab", "ups-adaptor-cpl.csv")

spec = importlib.util.spec_from_file_location(
    "gen_bom", os.path.join(ROOT, "tools", "gen-bom.py"))
gen_bom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_bom)


def main():
    if not os.path.exists(CPL):
        print("  FAIL run tools/pack-fab.sh first")
        return 1

    if not os.path.exists(KICAD_POS):
        # The raw position export is gitignored ON PURPOSE -- it carries KiCad
        # angles, and uploading it in place of the real CPL fits eight parts the
        # wrong way round. So it is absent on a fresh clone and in CI, and this
        # check cannot compare against it there. Say so rather than failing: an
        # error nobody can act on trains people to ignore errors.
        print("  SKIP no raw position export; it is gitignored by design.")
        print("       Run tools/pack-fab.sh to regenerate it, and this check")
        print("       compares every CPL angle against KiCad's own.")
        return 0

    raw = {r["Ref"]: float(r["Rot"]) for r in csv.DictReader(open(KICAD_POS))}
    out = {r["Designator"]: float(r["Rotation"])
           for r in csv.DictReader(open(CPL))}
    over = gen_bom.ROTATION_OVERRIDE

    fails = 0
    if gen_bom.APPLY_VENDOR_ROTATIONS:
        print("  FAIL gen-bom.APPLY_VENDOR_ROTATIONS is on. That table put Q2")
        print("       90 degrees out in JLC's preview. Turn it off and override")
        print("       individual parts against what the preview shows.")
        fails += 1

    for ref, (angle, why) in sorted(over.items()):
        if not why or len(why) < 15:
            print("  FAIL override %s = %g has no stated evidence" % (ref, angle))
            fails += 1

    bad = []
    for ref, got in sorted(out.items()):
        want = raw.get(ref)
        if want is None:
            print("  FAIL %s is in the CPL but not in KiCad's position file" % ref)
            fails += 1
            continue
        if ref in over:
            want = over[ref][0] % 360
        if abs(((got - want + 180) % 360) - 180) > 0.01:
            bad.append((ref, got, want))
    if bad:
        fails += 1
        for ref, got, want in bad:
            print("  FAIL %s is %g in the CPL, expected %g" % (ref, got, want))
    else:
        print("  ok   all %d CPL angles are KiCad's own%s"
              % (len(out), ", with %d override(s)" % len(over) if over else
                 " -- none overridden"))
        for ref, (angle, why) in sorted(over.items()):
            print("       %-4s forced to %3g: %s" % (ref, angle, why))

    # The positions have to be centroids, not anchors: gen-cpl-pos.py writes
    # them that way and this is the cheap check that it ran.
    pos = {r["Ref"]: (float(r["PosX"]), float(r["PosY"]))
           for r in csv.DictReader(open(KICAD_POS))}
    u1 = pos.get("U1")
    if u1 and abs(u1[1] + 70.22) > 0.05:
        print("  FAIL U1's position is %.2f, not its pad centroid -70.22."
              % u1[1])
        print("       hardware/fab/ups-adaptor-cpl.csv came from kicad-cli, which")
        print("       writes the ANCHOR. Run tools/gen-cpl-pos.py instead.")
        fails += 1
    elif u1:
        print("  ok   positions are pad centroids (U1 at %.2f, 3.77 mm from its"
              " anchor)" % u1[1])

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
