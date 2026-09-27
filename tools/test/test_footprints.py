#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every component must carry a footprint, and that footprint must exist.

A footprint name typo is invisible until layout, where it stops the job dead.
Worse, a plausible-but-wrong name (right family, wrong pitch or variant) survives
into fab and produces a board whose parts do not fit. This is cheap to check now
and expensive to discover later.
"""
import os
import sys, re, subprocess, sys, tempfile

# Locate KiCad without hardcoding one machine's install path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ""))
import kicad_paths

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KC = kicad_paths.cli()
FPDIR = kicad_paths.footprints()
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")

# Footprints tied to a specific orderable part rather than a generic package.
# These must be matched against what is actually bought before the board is made.
PART_SPECIFIC = ("USB_C_Receptacle", "USB_A_")


def main():
    tmp = tempfile.mktemp(suffix=".net")
    subprocess.run([KC, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "-o", tmp, SCH], capture_output=True, text=True)
    if not os.path.exists(tmp):
        print("  FAIL netlist export failed")
        return 1

    text = open(tmp).read()
    comps = re.findall(r'\(comp\s*\(ref "([^"]+)"\)(.*?)(?=\n\t\t\(comp|\n\t\)\n)', text, re.S)
    fails = 0
    checked = 0
    flagged = []

    for ref, body in comps:
        if ref.startswith("#"):
            continue
        m = re.search(r'\(footprint "([^"]*)"\)', body)
        fp = m.group(1) if m else ""
        if not fp:
            print("  FAIL %-5s has no footprint" % ref)
            fails += 1
            continue
        if ":" not in fp:
            print("  FAIL %-5s footprint %r is not Lib:Name" % (ref, fp))
            fails += 1
            continue
        lib, name = fp.split(":", 1)
        path = os.path.join(FPDIR, lib + ".pretty", name + ".kicad_mod")
        if not os.path.exists(path):
            print("  FAIL %-5s footprint not found: %s" % (ref, fp))
            fails += 1
            continue
        checked += 1
        if any(k in name for k in PART_SPECIFIC):
            flagged.append((ref, fp))

    print("  ok   %d footprints assigned and present in the KiCad libraries" % checked)

    if flagged:
        print("  WARN %d footprints are tied to a specific orderable part and must be"
              % len(flagged))
        print("       matched against what is actually bought before fab:")
        for ref, fp in flagged:
            print("         %-4s %s" % (ref, fp))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
