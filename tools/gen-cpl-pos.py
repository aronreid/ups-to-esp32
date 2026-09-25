#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the position file gen-bom.py reads, using CENTROIDS not anchors.

`kicad-cli pcb export pos` writes each footprint's ANCHOR -- the origin the
library author chose. A pick-and-place machine puts the part's centroid at the
coordinate it is given, so wherever the anchor is not the centre of the pads,
the part lands off its own pads by exactly that difference.

On this board three parts differ, and they are the three a screenshot of JLC's
preview showed sitting wrong:

    U1  ESP32-S3-WROOM-1   3.77 mm   anchor is the module body centre, but the
                                     antenna end carries no pads, so the pad
                                     array's centre is 3.77 mm further down
    J2  XKB USB-A          2.49 mm   anchor is the body centre, pads are at one end
    J1  HRO USB-C          1.46 mm   same

For every other part on the board the two agree to better than 0.01 mm, so this
changes those three and nothing else. Same columns and same sign convention as
kicad-cli's export (Y negative, axis down), because gen-bom.py reads it.

Run with KiCad's Python:  $KICAD_PY tools/gen-cpl-pos.py
"""
import csv
import os
import sys

import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")
OUT = os.path.join(ROOT, "hardware", "fab", "ups-adaptor-cpl.csv")


def mm(v):
    return pcbnew.ToMM(v)


def centroid(fp):
    """Centre of the pad array's bounding box, in board mm."""
    xs, ys = [], []
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        xs += [mm(bb.GetLeft()), mm(bb.GetLeft()) + mm(bb.GetWidth())]
        ys += [mm(bb.GetTop()), mm(bb.GetTop()) + mm(bb.GetHeight())]
    if not xs:
        p = fp.GetPosition()
        return mm(p.x), mm(p.y)
    return (min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0


def main():
    board = pcbnew.LoadBoard(PCB)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    moved = []
    rows = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        cx, cy = centroid(fp)
        ax, ay = mm(fp.GetPosition().x), mm(fp.GetPosition().y)
        d = max(abs(cx - ax), abs(cy - ay))
        if d >= 0.01:
            moved.append((ref, d, cx - ax, cy - ay))
        rows.append((ref, fp.GetValue(),
                     fp.GetFPID().GetUniStringLibId().split(":")[-1],
                     cx, -cy, fp.GetOrientationDegrees(),
                     "bottom" if fp.IsFlipped() else "top"))
    rows.sort(key=lambda r: (r[0].rstrip("0123456789"),
                            int(r[0][len(r[0].rstrip("0123456789")):] or 0)))
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Ref", "Val", "Package", "PosX", "PosY", "Rot", "Side"])
        for ref, val, pkg, x, y, rot, side in rows:
            w.writerow([ref, val, pkg, "%.6f" % x, "%.6f" % y,
                        "%.6f" % rot, side])
    print("wrote %s: %d placements at their PAD CENTROIDS"
          % (os.path.relpath(OUT, ROOT), len(rows)))
    for ref, d, dx, dy in sorted(moved, key=lambda t: -t[1]):
        print("   %-4s moved %.2f mm from KiCad's anchor (%+.2f, %+.2f) -- the"
              " anchor is not the centre of its pads" % (ref, d, dx, dy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
