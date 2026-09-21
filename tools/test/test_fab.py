#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The committed fab package still matches the committed board.

hardware/fab/ is checked in so the order files can be uploaded from any machine
without KiCad. That is the exact situation where a generated artifact rots: the
board gets edited, nothing regenerates the zip, and every other gate still
passes because none of them looks at it. Somebody then uploads last week's
silkscreen.

So this asserts the one thing that matters -- the zip was plotted from THIS
board -- by hashing the .kicad_pcb and comparing against what pack-fab.sh
recorded. Timestamps cannot do this: git does not preserve mtimes, so after a
clone every file looks equally fresh.
"""
import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")
FAB = os.path.join(ROOT, "hardware", "fab")
ZIP = os.path.join(FAB, "ups-adaptor-gerbers.zip")
STAMP = os.path.join(FAB, "SOURCE.sha256")

# What JLC needs, and what a fab importer will look for.
WANT = ["F_Cu", "B_Cu", "F_Mask", "B_Mask", "F_Silkscreen", "B_Silkscreen",
        "F_Paste", "B_Paste", "Edge_Cuts", "job", "NPTH", "PTH"]


def main():
    fails = 0
    for path in (ZIP, STAMP):
        if not os.path.exists(path):
            print("  FAIL %s is missing -- run tools/pack-fab.sh"
                  % os.path.relpath(path, ROOT))
            return 1

    want = open(STAMP).read().strip()
    got = hashlib.sha256(open(PCB, "rb").read()).hexdigest()
    if want != got:
        fails += 1
        print("  FAIL the committed gerbers were plotted from a DIFFERENT board")
        print("       board now  %s" % got[:16])
        print("       zip built from %s" % want[:16])
        print("       Run tools/pack-fab.sh and commit the result, or the next")
        print("       order uploads artwork that is not this board.")
    else:
        print("  ok   gerbers were plotted from this exact board (%s)" % got[:16])

    names = zipfile.ZipFile(ZIP).namelist()
    missing = [w for w in WANT if not any(w in n for n in names)]
    if missing:
        fails += 1
        print("  FAIL the zip is missing %d layer(s): %s"
              % (len(missing), ", ".join(missing)))
    else:
        print("  ok   zip carries all %d manufacturing layers plus both drill files"
              % (len(WANT) - 3))

    # Documentation layers in a fab zip are at best ignored and at worst
    # imported as copper.
    junk = [n for n in names
            if any(x in n for x in ("Courtyard", "Fab", "Adhesive", "Margin",
                                    "User_", "Comments", "Drawings", "Eco"))]
    if junk:
        fails += 1
        print("  FAIL documentation layers in the fab zip: %s" % ", ".join(junk))
    else:
        print("  ok   no documentation layers in the zip -- nothing a fab importer")
        print("       can mistake for copper")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
