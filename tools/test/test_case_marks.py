#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Engraved text on the case must read true, not mirrored, where it is seen.

Written after the underside's FCC/IC lines were generated mirrored twice by
reasoning about coordinate frames. This checks the geometry itself instead:
it slices the engraving out of the real solids, views it the way a person does
-- the lid from above, the tray's underside with the case turned over -- and
compares it with the same words laid out plainly. It must overlap the plain
text almost exactly, and must NOT overlap the mirror image.

The lid is the control: its labels are known to read true from above. If the
viewing transform here were wrong, the lid would fail too.

Needs manifold3d, numpy and matplotlib (CI installs them; locally, the case
venv). Without them it reports SKIP rather than passing silently.
"""
import importlib.util, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
try:
    from manifold3d import CrossSection, OpType
except ImportError:
    print("SKIP: manifold3d not installed (pip install manifold3d numpy matplotlib)")
    sys.exit(0)

spec = importlib.util.spec_from_file_location("gen_case", ROOT / "tools/gen-case.py")
gc = importlib.util.module_from_spec(spec); spec.loader.exec_module(gc)


def iou(a, b):
    inter = (a ^ b).area()
    return inter / ((a + b).area() or 1)


def mirror_x(cs):
    return cs.mirror((1, 0))


def view_slice(solid, z, flip):
    """The engraving's outline at height z, as seen: from above, (u, -v);
    turned over and seen from below, (-u, -v)."""
    cs = solid.slice(z)
    cs = cs.mirror((0, 1))                    # (u, v) -> (u, -v): the top view
    return cs.mirror((1, 0)) if flip else cs  # turned over: u flips as well


fails = 0

# --- the lid, from above: the control -------------------------------------
lid = gc.lid_labels(gc.H, "plain")
seen = view_slice(lid, gc.H - gc.LABEL_DEPTH / 2, flip=False)
want = None
for text, u, v, cap, align, covers in gc.LABELS:
    if covers == "plain" or covers == "both":
        cs = gc.text_cs(text, cap)
        x0, y0, x1, y1 = cs.bounds()
        dx = {"l": -x0, "r": -x1, "c": -(x0 + x1) / 2}[align]
        cs = cs.translate((u + dx, -v - cap / 2 - y0))
        want = cs if want is None else want + cs
s_true, s_mirror = iou(seen, want), iou(seen, mirror_x(want).translate((0, 0)))
ok = s_true > 0.95
print(f"{'ok  ' if ok else 'FAIL'} lid labels from above read true (overlap {s_true:.3f})")
fails += not ok

# --- the tray's underside, turned over -------------------------------------
reg = gc.reg_marks()
seen = view_slice(reg, gc.REG_DEPTH / 2, flip=True)
for lines, u in gc.REG_BLOCKS:
    # the block as it should read, laid out plainly, then turned either way so
    # its lines run along the tray: a reader turns the case to suit
    block = None
    for k, text in enumerate(lines):
        cs = gc.text_cs(text, gc.REG_CAP)
        x0, _, x1, _ = cs.bounds()
        cs = cs.translate((-(x0 + x1) / 2, -k * gc.REG_CAP * gc.REG_LEADING))
        block = cs if block is None else block + cs
    x0, y0, x1, y1 = block.bounds()
    block = block.translate((-(x0 + x1) / 2, -(y0 + y1) / 2))
    # where this block sits in the view from below
    cu, cv = -u, -(gc.OV0 + gc.OV1) / 2
    region = CrossSection.square((gc.REG_CAP * 6, 200), True).translate((cu, cv))
    part = seen ^ region
    best_true = max(iou(part, block.rotate(a).translate((cu, cv))) for a in (90, -90))
    best_mirror = max(iou(part, mirror_x(block).rotate(a).translate((cu, cv))) for a in (90, -90))
    ok = best_true > 0.95 and best_mirror < 0.6
    print(f"{'ok  ' if ok else 'FAIL'} underside '{lines[0]} {lines[1]}' reads true from below "
          f"(overlap {best_true:.3f}; as a mirror image {best_mirror:.3f})")
    fails += not ok

sys.exit(1 if fails else 0)
