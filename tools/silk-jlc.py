#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Raise silkscreen to JLCPCB's published minimums.

Stock KiCad footprints draw silk at 0.12 mm and set reference text 0.9 mm high.
JLCPCB's minimums are **0.15 mm line width** and **1.0 mm text height**; below
those, silk comes out broken or illegible, or the fab simply drops it. Nothing
in DRC catches this by default -- `min_text_height` and `min_text_thickness` in
the board rules apply to text on COPPER, not silk.

Normalising here beats hand-editing 38 stock footprints, and beats writing a
house footprint library for a board whose silk is otherwise fine.

This is a separate script rather than another phase of silk-pcb.py because
pcbnew's second LoadBoard() in one process hands back a raw SwigPyObject with
no methods on it, and silk-pcb.py has to reload after adding and removing
drawings.

Run after silk-pcb.py.
"""
import math
import os
import sys

import pcbnew

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

MIN_W = 0.15       # mm, silkscreen line width
MIN_H = 1.0        # mm, silkscreen text height
SILK = (pcbnew.F_SilkS, pcbnew.B_SilkS)


def main():
    board = pcbnew.LoadBoard(PCB)
    state = {"widened": 0, "enlarged": 0}

    def fix_text(it):
        if pcbnew.ToMM(it.GetTextHeight()) < MIN_H:
            it.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(MIN_H),
                                           pcbnew.FromMM(MIN_H)))
            state["enlarged"] += 1
        if pcbnew.ToMM(it.GetTextThickness()) < MIN_W:
            it.SetTextThickness(pcbnew.FromMM(MIN_W))
            state["widened"] += 1

    def fix_shape(it):
        if it.GetWidth() > 0 and pcbnew.ToMM(it.GetWidth()) < MIN_W:
            it.SetWidth(pcbnew.FromMM(MIN_W))
            state["widened"] += 1

    for d in board.GetDrawings():
        if d.GetLayer() not in SILK:
            continue
        if isinstance(d, pcbnew.PCB_TEXT):
            fix_text(d)
        elif hasattr(d, "SetWidth"):
            fix_shape(d)

    for fp in board.GetFootprints():
        for it in list(fp.GraphicalItems()) + [fp.Reference(), fp.Value()]:
            if it.GetLayer() not in SILK:
                continue
            if isinstance(it, (pcbnew.PCB_TEXT, pcbnew.PCB_FIELD)):
                if it.IsVisible():
                    fix_text(it)
            elif hasattr(it, "SetWidth"):
                fix_shape(it)

    # --- keep the enlarged text legible -------------------------------
    #
    # Making text bigger makes it collide. Two collisions actually lose
    # information and are worth moving for: text clipped by the board edge is
    # cut off, and text over a pad is clipped by the solder mask opening.
    # Silk crossing another part's outline is untidy but still readable, so it
    # is left alone rather than shuffled around a dense board.
    bb = board.GetBoardEdgesBoundingBox()
    eL, eT = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
    eR, eB = pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())

    pads = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            pb = pad.GetBoundingBox()
            pads.append((pcbnew.ToMM(pb.GetLeft()), pcbnew.ToMM(pb.GetTop()),
                         pcbnew.ToMM(pb.GetRight()), pcbnew.ToMM(pb.GetBottom())))

    GAP = 0.15

    def box_of(it):
        b = it.GetBoundingBox()
        return (pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop()),
                pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom()))

    def bad(bx, dx=0.0, dy=0.0):
        x0, y0, x1, y1 = bx[0] + dx, bx[1] + dy, bx[2] + dx, bx[3] + dy
        if x0 < eL + GAP or x1 > eR - GAP or y0 < eT + GAP or y1 > eB - GAP:
            return True
        for px0, py0, px1, py1 in pads:
            if x0 < px1 + GAP and x1 > px0 - GAP and y0 < py1 + GAP and y1 > py0 - GAP:
                return True
        return False

    texts = [d for d in board.GetDrawings()
             if d.GetLayer() in SILK and isinstance(d, pcbnew.PCB_TEXT)]
    for fp in board.GetFootprints():
        for it in (fp.Reference(), fp.Value()):
            if it.GetLayer() in SILK and it.IsVisible():
                texts.append(it)

    # Other silk text counts as an obstacle too. Without this the nudge solved
    # one collision by creating another: J5's pin labels are 2.54mm apart, and
    # pushing one clear of its header pad landed it on top of its neighbour.
    others = []
    for it in texts:
        others.append(list(box_of(it)))

    def bad_text(idx, bx, dx, dy):
        x0, y0, x1, y1 = bx[0] + dx, bx[1] + dy, bx[2] + dx, bx[3] + dy
        for j, ob in enumerate(others):
            if j == idx:
                continue
            if x0 < ob[2] + GAP and x1 > ob[0] - GAP and y0 < ob[3] + GAP and y1 > ob[1] - GAP:
                return True
        return False

    moved = stuck = 0
    for idx, it in enumerate(texts):
        bx = box_of(it)
        if not bad(bx) and not bad_text(idx, bx, 0.0, 0.0):
            continue
        placed = False
        r = 0.3
        while r <= 3.0 and not placed:
            for k in range(16):
                ang = 2 * 3.14159265 * k / 16.0
                dx, dy = r * math.cos(ang), r * math.sin(ang)
                if bad(bx, dx, dy) or bad_text(idx, bx, dx, dy):
                    continue
                others[idx] = [bx[0] + dx, bx[1] + dy, bx[2] + dx, bx[3] + dy]
                pos = it.GetPosition()
                it.SetPosition(pcbnew.VECTOR2I(pos.x + pcbnew.FromMM(dx),
                                               pos.y + pcbnew.FromMM(dy)))
                moved += 1
                placed = True
                break
            r += 0.1
        if not placed:
            stuck += 1

    pcbnew.SaveBoard(PCB, board)
    print("raised %d silk lines to %.2fmm and %d texts to %.1fmm for JLC"
          % (state["widened"], MIN_W, state["enlarged"], MIN_H))
    print("nudged %d silk texts off pads and the board edge%s"
          % (moved, ", %d had nowhere to go" % stuck if stuck else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
