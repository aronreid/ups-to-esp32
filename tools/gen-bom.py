#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the two files JLCPCB's assembly order actually takes.

    hardware/bom/bom.csv   Comment, Designator, Footprint, LCSC Part #
    hardware/bom/cpl.csv   Designator, Mid X, Mid Y, Layer, Rotation

WHY THIS EXISTS

hardware/bom/bom.csv used to be a hand-exported file that nothing regenerated.
By the time anyone looked, it still listed the ESP32-S3 N8 (now N4), a
through-hole Wuerth USB-A (now the XKB), a 500 mA fuse (now 2 A), a deleted J4,
22uF for C7 (now 100uF) -- and the AP2161W, the active-LOW load switch whose
replacement was the single most important fix of the whole review. Uploading
it would have ordered the inverted part. It also had no LCSC column, so it was
not a JLC BOM at all.

The BOM is now derived from the same DESIGN table that generates the schematic,
so it cannot disagree with it, and build-pcb.sh runs this at the end of every
build. tools/test/test_bom.py fails the harness if the files drift anyway.

LCSC codes: EVERY line carries one, each checked against a live LCSC page
(docs/BOM.md). Passives used to be left blank so JLC's matcher could pick a
current Basic part by value and package. Left blank, JLC's matcher resolved
every single resistor line to an 01005 -- because KiCad's footprint
is named "R_0402_1005Metric" and JLC read the 0402 as METRIC, which is imperial
01005, a quarter the length of the pads. All five lines came back out of stock,
Extended, and below Economic PCBA's 0402 floor. Four capacitor lines resolved to
nothing at all. Nothing in this repo could have caught it: the BOM was correct,
the footprints were correct, and the substitution happened inside JLC's cart.

So the convention is reversed for passives: the code is pinned here and
tools/test/test_bom.py fails if any line is blank.

Through-hole J3 and J5 are left OUT. JLC would fit them by hand ($3.50 labour
plus $0.0173 a joint); this project's first rule is low cost and eleven joints
is five minutes with an iron. Pass --with-tht to include them.
"""
import csv
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "hardware", "bom")
POS = os.path.join(ROOT, "hardware", "fab", "ups-adaptor-cpl.csv")

# Every part, verified against a live LCSC page -- see docs/BOM.md.
LCSC = {
    # --- passives. Imperial 0402 = metric 1005; the Uniroyal 0402WGF series and
    # the Samsung CL05 series are both imperial 0402, both JLC Basic, both
    # stocked in the millions. Do not let these go blank again.
    "1k": "C11702",              # 0402WGF1001TCE  1k  1%  62.5mW
    "10k": "C25744",             # 0402WGF1002TCE
    "4.7k": "C25900",            # 0402WGF4701TCE
    "5.1k": "C25905",            # 0402WGF5101TCE  -- CC pulldowns, 5% would do
    "100k": "C25741",            # 0402WGF1003TCE
    # One 100nF part covers both rails. C8 is specified 25V but sits on VBUS_A,
    # a 5V rail, so a 16V X7R is 3.2x margin and saves a unique line.
    "100nF 16V": "C1525",        # CL05B104KO5NNNC 100nF 16V X7R 0402
    "100nF 25V": "C1525",
    "1uF 16V": "C52923",         # CL05A105KA5NQNC 1uF 25V X5R 0402
    # One 22uF part for both. Specified 16V (C4, +5V) and 6.3V (C5, +3V3); a
    # 25V part in the same 0805 satisfies both AND holds more capacitance under
    # DC bias than either, so the derating in test_power is now pessimistic.
    "22uF 16V": "C45783",        # CL21A226MAQNNNE 22uF 25V X5R 0805
    "22uF 6.3V": "C45783",
    "100uF 6.3V": "C15008",      # CL31A107MQHNNNE 100uF 6.3V X5R 1206
    # --- everything else
    "ESP32-S3-WROOM-1-N4": "C2913197",
    "CH340C": "C84681",
    # The "H" suffix is load-bearing: AP2114HA is the same SOT-223 with pin 1
    # VIN and pin 2 GND, and is also stocked at LCSC.
    "AP2114H-3.3": "C150716",
    "AP2171W": "C110466",
    "USBLC6-2SC6": "C7519",
    "MMBT3904": "C20526",
}
# These cannot be left to the matcher: "RESET", "green" and "2A hold" are not
# values it can resolve, and an unresolved line silently drops the part from
# the assembly order. Each is a specific part whose land pattern or rating the
# board depends on.
LCSC_BY_REF = {
    "J1": "C165948",     # HRO TYPE-C-31-M-12
    "J2": "C2880618",    # XKB U231-091N-4BLRA00-S
    "SW1": "C318884",    # XKB TS-1187A-B-A-B, JLC Basic; footprint is its own
    "SW2": "C318884",
    # KENTO KT-0603Y yellow, the same family and land pattern as D4's red.
    # Was Everlight C72038, which went out of stock at LCSC -- JLC's preview
    # reported a shortfall. NOT green: a green 0603 is
    # Vf 3.3V on a 3.3V rail, see gen-schematic.py.
    "D3": "C2287",
    "D4": "C2286",       # KENTO KT-0603R red, JLC Basic
    "F1": "C545216",     # PTTC SMD1206P200TF: 2A hold, 3.5A trip, 6V
}
THT = {"J3", "J5"}


def design():
    spec = importlib.util.spec_from_file_location(
        "gen_schematic", os.path.join(HERE, "gen-schematic.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.DESIGN


def rows(with_tht=False):
    groups = {}
    for entry in design():
        ref, _lib, symbol, value, footprint = entry[:5]
        if ref in THT and not with_tht:
            continue
        code = LCSC_BY_REF.get(ref) or LCSC.get(value) or LCSC.get(symbol) or ""
        key = (value, footprint.split(":")[-1], code)
        groups.setdefault(key, []).append(ref)

    def refkey(r):
        head = r.rstrip("0123456789")
        return (head, int(r[len(head):] or 0))
    out = []
    for (value, fp, code), refs in groups.items():
        refs.sort(key=refkey)
        out.append((refkey(refs[0]), value, ",".join(refs), fp, code))
    out.sort()
    return [r[1:] for r in out]


def write_bom(with_tht=False):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "bom.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for r in rows(with_tht):
            w.writerow(r)
    return path


ROTDB = os.path.join(HERE, "test", "jlc_cpl_rotations.csv")

# ---------------------------------------------------------------- rotation
# The CPL carries KiCad's own angles. It used to carry the JLCKicadTools
# table's "reel angle" corrections instead. JLC's assembly
# preview showed that table putting Q2 ninety degrees out: the board's SOT-23
# pads sit two-down/one-up, and the model's leads came out left and right.
#
# The table is community-maintained and undated. We cannot verify it, and a
# correction we cannot verify is not safer than no correction -- it is the same
# gamble with extra steps. So nothing is applied by default, and each part JLC's
# preview actually shows wrong gets an entry below, with the evidence written
# next to it. The preview renders JLC's own library model at our angle over our
# gerbers: if the model's leads land on the pads, the angle is right, and that
# is a thing a human can see.
APPLY_VENDOR_ROTATIONS = False

# ref -> (absolute angle for the CPL, what was seen that justifies it)
#
# Every entry here was decided by looking at JLC's preview, which draws JLC's
# own library model at our angle on top of our gerbers. "The model's leads sit
# on the pads" is the test. Two uploads -- one with the vendored
# table applied, one with raw KiCad angles -- bracket most of these: a part that
# was right in one and wrong in the other tells you which value is correct.
ROTATION_OVERRIDE = {
    # Board pads run E-W (8 north, 8 south). CPL 0 drew it E-W, CPL 90 drew it
    # N-S. The vendored table's +270 for ^SOIC- is right.
    "U2": (0, "preview: CPL 0 drew it E-W over its E-W pad rows; CPL 90 N-S"),
    # 3 leads west, tab east. CPL 0 was visibly wrong in the second upload.
    "U3": (180, "preview: CPL 0 wrong; 180 matches 3-leads-west, tab-east"),
    # SOT-23-5 and -6 behaved the same in the first upload: CPL 270 put the
    # leads west and east, which is how the board has them.
    "U4": (270, "preview: CPL 270 put its leads W/E as the board has them"),
    "D1": (270, "preview: CPL 270 drew 3 leads W and 3 E; CPL 0 drew them N/S"),
    "D2": (270, "preview: CPL 270 drew 3 leads W and 3 E; CPL 0 drew them N/S"),
    # The 3-lead SOT-23 is the family the vendored table gets WRONG. Its -90
    # left Q2 ninety degrees out; KiCad's own angle left it a hundred and
    # eighty out, which is the reading that identifies the real offset: JLC's
    # SOT-23 reel presents the part 180 degrees from KiCad's footprint.
    "Q1": (180, "preview: KiCad angle 0 is 180 out, as Q2 was at 90"),
    "Q2": (270, "preview: at KiCad's 90 the model was flipped; board is 2 S, 1 N"),
    # J1 is NOT here on purpose. The vendored table has a rule for this exact
    # HRO part -- not a family, the part -- and it says +180. The preview drew
    # it upside down at 180 and correct at KiCad's own 0. That is the last
    # entry in that table this board trusted, and it was wrong too, which is
    # the whole argument for overriding per part against something you can see.
}


def _corrections():
    """JLC's pick-and-place orients a part the way its REEL presents it. KiCad
    exports the rotation of the footprint. For most two-terminal passives those
    agree; for a lot of ICs they do not, and nothing in KiCad, in DRC or in the
    gerbers knows the difference.

    This is the table the JLCKicadTools project maintains for exactly that gap:

        https://github.com/matthewlai/JLCKicadTools
        jlc_kicad_tools/cpl_rotations_db.csv

    Vendored rather than fetched, so an order does not depend on a URL being up,
    and so a change to it shows as a diff. It is community-maintained, not
    official: JLC's own assembly preview is still the authority. What it buys is
    a SHORT list to check there instead of all 36 placements."""
    out = []
    with open(ROTDB) as f:
        for row in csv.reader(f):
            if not row or row[0].lower().startswith("footprint") or len(row) < 2:
                continue
            try:
                rot = int(float(row[1]))
            except ValueError:
                continue
            ox = float(row[2]) if len(row) > 2 and row[2].strip() else 0.0
            oy = float(row[3]) if len(row) > 3 and row[3].strip() else 0.0
            out.append((row[0].strip(), rot, ox, oy))
    return out


def write_cpl(with_tht=False):
    """KiCad's position file, in JLC's column names. KiCad writes Y negative
    (its axis points down); JLC's importer expects exactly that convention from
    KiCad exports, so the values pass through untouched."""
    if not os.path.exists(POS):
        return None
    path = os.path.join(OUT, "cpl.csv")
    with open(POS, newline="") as f, open(path, "w", newline="") as g:
        w = csv.writer(g)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        corr = _corrections() if APPLY_VENDOR_ROTATIONS else []
        fps = {}
        for e in design():
            fps[e[0]] = e[4].split(":")[-1]
        applied = []
        written = 0
        for row in csv.DictReader(f):
            ref = row["Ref"]
            if ref in THT and not with_tht:
                continue
            rot = float(row["Rot"])
            fp = fps.get(ref, "")
            for pat, dr, ox, oy in corr:
                try:
                    if not re.search(pat, fp):
                        continue
                except re.error:
                    continue
                if ox or oy:
                    raise SystemExit(
                        "%s matches %r which needs an X/Y offset (%+.2f,%+.2f). "
                        "This script only applies rotation -- add offset support "
                        "before ordering." % (ref, pat, ox, oy))
                applied.append((ref, fp, rot, dr, (rot + dr) % 360))
                rot = (rot + dr) % 360
                break
            if ref in ROTATION_OVERRIDE:
                want, why = ROTATION_OVERRIDE[ref]
                applied.append((ref, fp, rot, want - rot, want % 360))
                rot = want % 360
            w.writerow([ref, "%.4fmm" % float(row["PosX"]),
                        "%.4fmm" % float(row["PosY"]),
                        "Top" if row["Side"].lower() == "top" else "Bottom",
                        "%g" % rot])
            written += 1
        if applied:
            print("  %d rotation(s) changed from KiCad's angle:" % len(applied))
            for ref, fp, was, dr, now in applied:
                print("     %-4s %-38s %3.0f %+5d -> %3.0f"
                      % (ref, fp[:38], was, dr, now))
        else:
            print("  every rotation is KiCad's own angle, uncorrected "
                  "(%d placements)" % written)
    return path


def main():
    with_tht = "--with-tht" in sys.argv
    b = write_bom(with_tht)
    n = len(rows(with_tht))
    blank = sum(1 for r in rows(with_tht) if not r[3])
    print("wrote %s: %d lines, %d with a verified LCSC code, %d left to JLC's "
          "Basic-library matcher" % (os.path.relpath(b, ROOT), n, n - blank, blank))
    c = write_cpl(with_tht)
    print("wrote %s" % os.path.relpath(c, ROOT) if c else
          "no position file yet -- run build-pcb.sh to produce the CPL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
