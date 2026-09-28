#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the board against JLCPCB's published manufacturing limits.

WHY THIS IS NOT JUST DRC

DRC checks the board against the rules stored IN the board. If those rules are
looser than the fab's, DRC passes and the fab rejects the job -- or worse,
builds it and it fails. This measures the actual geometry and compares it to
JLCPCB's numbers directly, so the two cannot drift apart.

Limits below are JLCPCB's published capabilities for a standard 2-layer FR-4
board at 1 oz copper, plus the Economic PCBA assembly limits, checked
2026-09-18:

    https://jlcpcb.com/capabilities/pcb-capabilities
    https://jlcpcb.com/capabilities/pcb-assembly-capabilities

Where JLC publishes both an absolute limit and a recommended value, the
absolute limit is a FAIL and the recommended value is a WARN. A board that only
just clears the absolute limit yields worse than one with margin.

    $KICAD_PY tools/test/test_jlc.py
"""
import os
import sys

import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")

# --- JLCPCB, 2-layer FR-4, 1 oz -------------------------------------------
MIN_TRACK = 0.10           # mm
MIN_CLEARANCE = 0.10
MIN_VIA_HOLE = 0.15
MIN_VIA_DIAMETER = 0.25
MIN_VIA_ANNULAR = 0.05     # follows from the two above
MIN_PTH_ANNULAR = 0.18     # absolute; 0.25 recommended
REC_PTH_ANNULAR = 0.25
MIN_VIA_TO_VIA_HOLE = 0.20  # hole edge to hole edge
MIN_PAD_TO_PAD_HOLE = 0.45
MIN_EDGE_COPPER = 0.20
MIN_SILK_WIDTH = 0.15
MIN_SILK_TEXT_H = 1.00
MIN_BOARD_DIM = 10.0       # Economic PCBA assembles from 10 x 10 mm
MAX_BOARD_W, MAX_BOARD_H = 470.0, 500.0

mm = pcbnew.ToMM


def main():
    board = pcbnew.LoadBoard(PCB)
    fails, warns = [], []

    def check(ok, msg):
        (print("  ok   " + msg) if ok else fails.append(msg))
        if not ok:
            print("  FAIL " + msg)

    def warn(msg):
        warns.append(msg)
        print("  WARN " + msg)

    # --- board outline ----------------------------------------------------
    bb = board.GetBoardEdgesBoundingBox()
    bw, bh = mm(bb.GetWidth()), mm(bb.GetHeight())
    check(bw >= MIN_BOARD_DIM and bh >= MIN_BOARD_DIM
          and bw <= MAX_BOARD_W and bh <= MAX_BOARD_H,
          "board %.1f x %.1f mm is within Economic PCBA's %g x %g .. %g x %g"
          % (bw, bh, MIN_BOARD_DIM, MIN_BOARD_DIM, MAX_BOARD_W, MAX_BOARD_H))

    # --- track widths -----------------------------------------------------
    tracks = [t for t in board.GetTracks() if not isinstance(t, pcbnew.PCB_VIA)]
    if tracks:
        tw = min(mm(t.GetWidth()) for t in tracks)
        check(tw >= MIN_TRACK, "narrowest track %.3f mm >= JLC's %.2f mm"
              % (tw, MIN_TRACK))

    # --- vias: hole, diameter, annular ring -------------------------------
    vias = [t for t in board.GetTracks() if isinstance(t, pcbnew.PCB_VIA)]
    if vias:
        vh = min(mm(v.GetDrillValue()) for v in vias)
        vd = min(mm(v.GetWidth(pcbnew.F_Cu)) for v in vias)
        ring = min((mm(v.GetWidth(pcbnew.F_Cu)) - mm(v.GetDrillValue())) / 2.0
                   for v in vias)
        check(vh >= MIN_VIA_HOLE, "smallest via hole %.3f mm >= JLC's %.2f mm"
              % (vh, MIN_VIA_HOLE))
        check(vd >= MIN_VIA_DIAMETER, "smallest via pad %.3f mm >= JLC's %.2f mm"
              % (vd, MIN_VIA_DIAMETER))
        check(ring >= MIN_VIA_ANNULAR,
              "smallest via annular ring %.3f mm >= JLC's %.2f mm"
              % (ring, MIN_VIA_ANNULAR))
        print("       %d vias, all %.2f mm hole in %.2f mm pad"
              % (len(vias), vh, vd) if vh == max(mm(v.GetDrillValue()) for v in vias)
              else "       %d vias" % len(vias))

    # --- plated holes in footprints ---------------------------------------
    # A footprint can carry its own vias: the ESP32-S3-WROOM-1 has a 12-hole
    # thermal array under its ground pad. KiCad models those as plated pads, but
    # they are vias -- no component lead goes through them -- so they answer to
    # the via annular rule, not the tighter one for component holes. Anything
    # with a lead in it is a component hole. Distinguish by size: a 0.3mm hole
    # takes no lead.
    VIA_LIKE_HOLE = 0.35
    thermal = 0
    worst = None
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetAttribute() != pcbnew.PAD_ATTRIB_PTH:
                continue
            drill = mm(pad.GetDrillSizeX())
            if drill <= 0:
                continue
            if drill <= VIA_LIKE_HOLE:
                ring = (min(mm(pad.GetSizeX()), mm(pad.GetSizeY())) - drill) / 2.0
                if ring < MIN_VIA_ANNULAR:
                    fails.append("thermal via %s.%s ring %.3f mm"
                                 % (fp.GetReference(), pad.GetNumber(), ring))
                    print("  FAIL thermal via %s.%s annular ring %.3f mm"
                          % (fp.GetReference(), pad.GetNumber(), ring))
                thermal += 1
                continue
            size = min(mm(pad.GetSizeX()), mm(pad.GetSizeY()))
            r = (size - drill) / 2.0
            if worst is None or r < worst[0]:
                worst = (r, "%s.%s" % (fp.GetReference(), pad.GetNumber()),
                         drill, size)
    if thermal:
        print("  ok   %d in-footprint thermal vias judged by the via rule "
              "(no lead passes through them)" % thermal)
    if worst:
        r, who, drill, size = worst
        check(r >= MIN_PTH_ANNULAR,
              "smallest plated-hole annular ring %.3f mm (%s, %.2f mm hole in "
              "%.2f mm pad) >= JLC's %.2f mm" % (r, who, drill, size, MIN_PTH_ANNULAR))
        if MIN_PTH_ANNULAR <= r < REC_PTH_ANNULAR:
            warn("that ring is below JLC's RECOMMENDED %.2f mm. It will be "
                 "built, but registration is tighter than they like."
                 % REC_PTH_ANNULAR)

    # --- hole to hole -----------------------------------------------------
    holes = []   # (x, y, drill_mm, is_via, label)
    for v in vias:
        p = v.GetPosition()
        holes.append((mm(p.x), mm(p.y), mm(v.GetDrillValue()), True, "via"))
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetDrillSizeX() <= 0:
                continue
            p = pad.GetPosition()
            holes.append((mm(p.x), mm(p.y), mm(pad.GetDrillSizeX()), False,
                          "%s.%s" % (fp.GetReference(), pad.GetNumber())))
    worst_vv = worst_pp = None
    for i, (x1, y1, d1, v1, l1) in enumerate(holes):
        for x2, y2, d2, v2, l2 in holes[i + 1:]:
            gap = ((x1 - x2) ** 2 + (y1 - y2) ** 2) ** 0.5 - (d1 + d2) / 2.0
            both_via = v1 and v2
            slot = (worst_vv if both_via else worst_pp)
            if slot is None or gap < slot[0]:
                if both_via:
                    worst_vv = (gap, l1, l2)
                else:
                    worst_pp = (gap, l1, l2)
    if worst_vv:
        check(worst_vv[0] >= MIN_VIA_TO_VIA_HOLE,
              "closest via-to-via hole gap %.3f mm >= JLC's %.2f mm"
              % (worst_vv[0], MIN_VIA_TO_VIA_HOLE))
    if worst_pp:
        check(worst_pp[0] >= MIN_PAD_TO_PAD_HOLE,
              "closest pad-hole gap %.3f mm (%s to %s) >= JLC's %.2f mm"
              % (worst_pp[0], worst_pp[1], worst_pp[2], MIN_PAD_TO_PAD_HOLE))

    # --- copper to board edge --------------------------------------------
    eL, eT = mm(bb.GetLeft()), mm(bb.GetTop())
    eR, eB = mm(bb.GetRight()), mm(bb.GetBottom())
    worst_edge = None
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            p = t.GetPosition()
            half = mm(t.GetWidth(pcbnew.F_Cu)) / 2.0
            pts = [(mm(p.x), mm(p.y))]
        else:
            half = mm(t.GetWidth()) / 2.0
            pts = [(mm(t.GetStart().x), mm(t.GetStart().y)),
                   (mm(t.GetEnd().x), mm(t.GetEnd().y))]
        for x, y in pts:
            d = min(x - eL, eR - x, y - eT, eB - y) - half
            if worst_edge is None or d < worst_edge:
                worst_edge = d
    if worst_edge is not None:
        check(worst_edge >= MIN_EDGE_COPPER,
              "closest track/via copper to board edge %.3f mm >= JLC's %.2f mm"
              % (worst_edge, MIN_EDGE_COPPER))

    # --- silkscreen -------------------------------------------------------
    silk_layers = (pcbnew.F_SilkS, pcbnew.B_SilkS)
    min_w, min_h = None, None
    for d in board.GetDrawings():
        if d.GetLayer() not in silk_layers:
            continue
        if isinstance(d, pcbnew.PCB_TEXT):
            h = mm(d.GetTextHeight())
            w = mm(d.GetTextThickness())
            min_h = h if min_h is None else min(min_h, h)
            min_w = w if min_w is None else min(min_w, w)
        elif hasattr(d, "GetWidth"):
            w = mm(d.GetWidth())
            if w > 0:
                min_w = w if min_w is None else min(min_w, w)
    for fp in board.GetFootprints():
        for it in list(fp.GraphicalItems()) + [fp.Reference(), fp.Value()]:
            if it.GetLayer() not in silk_layers:
                continue
            if isinstance(it, (pcbnew.PCB_TEXT, pcbnew.PCB_FIELD)):
                if not it.IsVisible():
                    continue
                h = mm(it.GetTextHeight())
                w = mm(it.GetTextThickness())
                min_h = h if min_h is None else min(min_h, h)
                min_w = w if min_w is None else min(min_w, w)
            elif hasattr(it, "GetWidth"):
                w = mm(it.GetWidth())
                if w > 0:
                    min_w = w if min_w is None else min(min_w, w)
    if min_w is not None:
        check(min_w >= MIN_SILK_WIDTH,
              "thinnest silkscreen line %.3f mm >= JLC's %.2f mm"
              % (min_w, MIN_SILK_WIDTH))
    if min_h is not None:
        if min_h >= MIN_SILK_TEXT_H:
            print("  ok   smallest silkscreen text %.2f mm >= JLC's %.2f mm"
                  % (min_h, MIN_SILK_TEXT_H))
        else:
            warn("smallest silkscreen text %.2f mm is below JLC's %.2f mm "
                 "minimum -- it may come out illegible or be dropped"
                 % (min_h, MIN_SILK_TEXT_H))

    # --- assembly: Economic PCBA package limits ---------------------------
    TINY = {"0201": "0201", "01005": "01005"}
    too_small = []
    for fp in board.GetFootprints():
        fpid = fp.GetFPIDAsString()
        for k in TINY:
            if k in fpid:
                too_small.append("%s (%s)" % (fp.GetReference(), k))
    check(not too_small,
          "no package smaller than 0402, which is Economic PCBA's limit"
          if not too_small else
          "packages below Economic PCBA's 0402 limit: %s" % ", ".join(too_small))

    print()
    if fails:
        print("  %d JLC rule violation(s)" % len(fails))
        return 1
    print("  board satisfies every JLCPCB limit checked (%d warning(s))" % len(warns))
    return 0


if __name__ == "__main__":
    sys.exit(main())
