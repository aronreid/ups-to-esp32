#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Silkscreen pass: identify the board, label the connectors, cut the clutter.

Silkscreen is the only documentation that arrives with the hardware. Two things
here are not cosmetic:

  - This board has TWO USB connectors that do opposite things. One supplies power
    to a UPS; the other takes power and carries the console. Plugging a host into
    the wrong one is the obvious mistake, and the board should say which is which.

  - Value fields on every part are clutter that pushes real markings off the
    board. References and polarity are what an assembler and a reviewer need.

Polarity markers are NOT generated here. The stock footprints carry their own,
and overriding them is how a TVS ends up with its "+" on the cathode. They are
flagged for a human to check against the render instead.

Run with KiCad's Python:  $KICAD_PY tools/silk-pcb.py
"""
import math
import os
import sys

import importlib.util
import pcbnew

# The label strips the placer keeps clear. Imported rather than duplicated:
# two copies of the same coordinates is how they drift apart.
_spec = importlib.util.spec_from_file_location(
    "place_pcb", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "place-pcb.py"))
_place = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_place)

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

TEXT_H = 0.9        # mm; JLC resolves ~0.8mm reliably
TEXT_W = 0.9
TEXT_T = 0.15


def mm(v):
    return pcbnew.ToMM(v)


def add_text(board, s, x, y, layer, h=TEXT_H, rot=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetLayer(layer)
    t.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    t.SetTextHeight(pcbnew.FromMM(h))
    t.SetTextWidth(pcbnew.FromMM(h))
    t.SetTextThickness(pcbnew.FromMM(TEXT_T))
    t.SetTextAngleDegrees(rot)
    if layer == pcbnew.B_SilkS:
        t.SetMirrored(True)
    board.Add(t)
    return t


LABELLED = {"J1", "J2", "J3", "J5"}   # parts that also get a functional label

# Silk earns its place or it goes. What a user needs from the top of this board
# is which pin is which, which port is which, and which light means what --
# everything else is noise that makes those harder to read. Passive designators
# are the worst of it: 21 of them, none of which tells anyone anything without
# the schematic open.
#
# None of this reaches assembly. JLC places parts from the CPL, which carries
# every designator whatever the silk says; tools/test/test_bom.py checks that.
# The cost is hand rework, where you now need the board file or a render to find
# R7. That is the trade being made deliberately.
# ref -> the pad number the dot marks. KiCad numbers an LED's CATHODE as pin 1,
# so one rule covers both "which end is the cathode" and "where is pin 1".
# On a diode a DOT is the wrong glyph. A dot beside a part reads as "pin 1",
# and on an LED pin 1 is the CATHODE -- the minus end -- so a reviewer who
# knows the capacitor convention, where the dot marks plus, reads it exactly
# backwards. These two get a bar across the cathode end instead, which is what
# a diode's own body marking means and cannot be read as anything else.
CATHODE_BAR = {"D3", "D4"}

POLARISED = {
    "D1": "1", "D2": "1",          # USBLC6 ESD arrays
    "D3": "1", "D4": "1",          # indicators: pin 1 is the cathode
    "U1": "1", "U2": "1",          # module, CH340C
    "U3": "1", "U4": "1",          # regulator, load switch
    "Q1": "1", "Q2": "1",          # auto-reset transistors
}

HIDE_REFDES_PREFIX = ("R", "C")       # passives: designator serves nobody here
NO_SILK = {"SW1", "SW2"}              # outline AND designator: the buttons are
                                      # self-evident, and RESET/BOOT say the rest

# Headers whose individual pins are labelled. An unlabelled 4-pin header invites
# someone to plug a module in backwards, and SSD1306 modules ship in more than
# one pin order -- reversed power usually kills them. An unlabelled 7-pin
# expansion header is simply unusable without the schematic to hand.
PIN_LABELLED = {"J3", "J5"}

# Net name -> what to print. Short enough to fit a 2.54mm pitch.
PIN_TEXT = {
    "+3V3": "3V3", "+5V": "5V", "GND": "GND", "SDA": "SDA", "SCL": "SCL",
    "EXP_IO10": "10", "EXP_IO11": "11", "EXP_IO12": "12", "EXP_IO13": "13",
}


def main():
    board = pcbnew.LoadBoard(PCB)
    bb = board.GetBoardEdgesBoundingBox()
    L, T = mm(bb.GetLeft()), mm(bb.GetTop())
    R, B = mm(bb.GetRight()), mm(bb.GetBottom())

    # PHASE 1 -- read and edit footprints, and capture the geometry needed
    # later as plain numbers.
    #
    # Removing anything from the board invalidates every footprint handle held
    # in Python: a later fp.Value() or fp.GetCourtyard() fails with
    # "SwigPyObject has no attribute ...". So all footprint work, and all
    # geometry capture, happens before a single removal. Same trap as
    # stitch-pcb.py.
    bb0 = board.GetBoardEdgesBoundingBox()
    bx0, by0 = mm(bb0.GetLeft()), mm(bb0.GetTop())

    def in_reserve(bbox):
        """True if a silk item overlaps a strip reserved for a label."""
        l, t = mm(bbox.GetLeft()) - bx0, mm(bbox.GetTop()) - by0
        r2, b2 = l + mm(bbox.GetWidth()), t + mm(bbox.GetHeight())
        return any(not (r2 <= a or l >= c2 or b2 <= b or t >= d)
                   for a, b, c2, d in _place.LABEL_RESERVES)

    boxes = {}
    nudged_out = 0
    dropped = 0
    strip = []         # (footprint, item) silk graphics to delete, collected
                       # first: removing while iterating invalidates the handles
    refboxes = []      # reference-designator silk, captured while handles live
    refowner = {}      # that box -> whose designator it is, so a polarity dot
                       # can evict it: the dot has one legal position, a
                       # designator has the whole board
    blockers = []      # what silk must actually avoid: pads and real bodies
    pins = {}          # ref -> [(pad number, net, x, y)], captured before any removal
    padrects = []      # every pad, individually. The per-footprint envelope is
                       # too coarse for a 0.5mm dot: on a SOT-23-6 it swallows
                       # the whole part and both diagonals beside pin 1.
    bodies = {}        # ref -> pad-derived bounding box, the part's real extent
    pin1 = {}          # ref -> (x, y, pad size) of the pin a dot must mark
    others = {}        # ref -> [(x, y)] of its OTHER pads: a dot nearer one of
                       # those than to pin 1 is worse than no dot at all
    hidden = moved = 0
    for fp in board.GetFootprints():
        ref = fp.GetReference()

        v = fp.Value()
        if v.IsVisible():
            v.SetVisible(False)
            hidden += 1

        if ref in NO_SILK:
            for it in list(fp.GraphicalItems()):
                if it.GetLayer() == pcbnew.F_SilkS:
                    strip.append((fp, it))
        # Hide the designator, but DO NOT skip the rest of this iteration: the
        # courtyard captured below is what the RESET and BOOT labels position
        # against, and an early `continue` here silently dropped both.
        no_ref = ref in NO_SILK or ref.startswith(HIDE_REFDES_PREFIX)
        if no_ref and fp.Reference().IsVisible():
            fp.Reference().SetVisible(False)
            dropped += 1

        r = fp.Reference()
        r.SetTextHeight(pcbnew.FromMM(TEXT_H))
        r.SetTextWidth(pcbnew.FromMM(TEXT_W))
        r.SetTextThickness(pcbnew.FromMM(TEXT_T))

        if ref in POLARISED:
            for pad in fp.Pads():
                pp = pad.GetPosition()
                if pad.GetNumber() == POLARISED[ref]:
                    pb = pad.GetBoundingBox()
                    pin1[ref] = (mm(pp.x), mm(pp.y),
                                 max(mm(pb.GetWidth()), mm(pb.GetHeight())))
                else:
                    others.setdefault(ref, []).append((mm(pp.x), mm(pp.y)))

        if ref in PIN_LABELLED:
            pins[ref] = sorted(
                ((pad.GetNumber(), pad.GetNetname(),
                  mm(pad.GetPosition().x), mm(pad.GetPosition().y))
                 for pad in fp.Pads()),
                key=lambda t: int(t[0]) if t[0].isdigit() else 99)

        # What actually stops silk being readable is copper and bodies, NOT the
        # courtyard. U1's courtyard is the 15mm antenna keepout -- a third of
        # the board, and a COPPER rule; silk inside it is perfectly fine. Using
        # courtyards here left no room for the button labels anywhere near the
        # buttons. Take the pads, and add the courtyard only when it is not
        # wildly larger than them.
        pb = None
        for pad in fp.Pads():
            pbb = pad.GetBoundingBox()
            q = (mm(pbb.GetLeft()), mm(pbb.GetTop()),
                 mm(pbb.GetLeft()) + mm(pbb.GetWidth()),
                 mm(pbb.GetTop()) + mm(pbb.GetHeight()))
            padrects.append((q[0] - 0.2, q[1] - 0.2, q[2] + 0.2, q[3] + 0.2))
            pb = q if pb is None else (min(pb[0], q[0]), min(pb[1], q[1]),
                                       max(pb[2], q[2]), max(pb[3], q[3]))
        if pb:
            blockers.append((pb[0] - 0.4, pb[1] - 0.4, pb[2] + 0.4, pb[3] + 0.4))
            # Same reasoning as above, for the polarity dot: "outside the part"
            # has to mean outside the BODY. U1's courtyard is the antenna
            # keepout, so a courtyard test leaves nowhere within 20mm of pin 1.
            bodies[ref] = pb
            cyb = fp.GetCourtyard(pcbnew.B_CrtYd if fp.IsFlipped() else pcbnew.F_CrtYd)
            if not cyb.IsEmpty():
                cb = cyb.BBox()
                cw, ch = mm(cb.GetWidth()), mm(cb.GetHeight())
                pw, ph = pb[2] - pb[0], pb[3] - pb[1]
                if cw * ch < 3.0 * max(pw * ph, 0.01):
                    cl, ct = mm(cb.GetLeft()), mm(cb.GetTop())
                    blockers.append((cl, ct, cl + cw, ct + ch))

        cy = fp.GetCourtyard(pcbnew.B_CrtYd if fp.IsFlipped() else pcbnew.F_CrtYd)
        if not cy.IsEmpty():
            c = cy.BBox()
            boxes[ref] = (mm(c.GetLeft()), mm(c.GetTop()),
                          mm(c.GetRight()), mm(c.GetBottom()))
            if no_ref:
                pass                    # no designator to reposition
            elif ref in LABELLED:
                # These parts get a functional label above them. Put the
                # designator below so the two do not sit on each other -- the
                # label is what a user reads, the designator is what an
                # assembler reads, and both need to be legible.
                r.SetPosition(pcbnew.VECTOR2I(
                    (c.GetLeft() + c.GetRight()) // 2,
                    c.GetBottom() + pcbnew.FromMM(0.9)))
                moved += 1
            elif ref in POLARISED and ref in pin1:
                # The dot has to sit beside pin 1 and be nearer to it than to
                # any other pad. On a 0.95mm-pitch SOT-23-6 that is a couple of
                # square millimetres, and D2's designator was sitting in it.
                # The designator can go anywhere; the dot cannot.
                _, py1, _ = pin1[ref]
                top = py1 < mm((c.GetTop() + c.GetBottom()) // 2)
                r.SetPosition(pcbnew.VECTOR2I(
                    (c.GetLeft() + c.GetRight()) // 2,
                    (c.GetBottom() + pcbnew.FromMM(0.9)) if top
                    else (c.GetTop() - pcbnew.FromMM(0.9))))
                moved += 1
            elif c.Contains(r.GetPosition()):
                r.SetPosition(pcbnew.VECTOR2I(
                    (c.GetLeft() + c.GetRight()) // 2,
                    c.GetTop() - pcbnew.FromMM(0.9)))
                moved += 1

            # A reserve exists so a functional label can sit there. The placer
            # keeps PARTS out of it, but reference designators are positioned
            # here, and C1's landed squarely in the strip meant for RESET --
            # which is why that label had "no room" beside a button with 2mm of
            # deliberately empty board under it. Push any offender to the far
            # side of its own courtyard.
            if not no_ref and in_reserve(r.GetBoundingBox()):
                r.SetPosition(pcbnew.VECTOR2I(
                    (c.GetLeft() + c.GetRight()) // 2,
                    c.GetBottom() + pcbnew.FromMM(0.9)))
                if in_reserve(r.GetBoundingBox()):
                    r.SetPosition(pcbnew.VECTOR2I(
                        c.GetLeft() - pcbnew.FromMM(1.2),
                        (c.GetTop() + c.GetBottom()) // 2))
                nudged_out += 1

    for fp, it in strip:
        fp.Remove(it)
    if strip:
        print("  removed %d silk outline items from %s"
              % (len(strip), ", ".join(sorted(NO_SILK))))

    # Reference designators are captured HERE, after every reposition above.
    # Capturing them inside that loop recorded where each one started, so the
    # label search saw stale boxes -- it refused to place RESET on 2mm of board
    # that had already been vacated for it.
    for fp in board.GetFootprints():
        r = fp.Reference()
        if r.IsVisible() and r.GetLayer() == pcbnew.F_SilkS:
            rb = r.GetBoundingBox()
            box = (mm(rb.GetLeft()), mm(rb.GetTop()),
                   mm(rb.GetLeft()) + mm(rb.GetWidth()),
                   mm(rb.GetTop()) + mm(rb.GetHeight()))
            refboxes.append(box)
            refowner[box] = fp.GetReference()

    # PHASE 2 -- remove text this script added previously, so it is re-runnable.
    # Board-level silk graphics are all ours: footprint silk lives inside the
    # footprint, so a loose F.SilkS circle here is a polarity dot from a
    # previous run and has to go before a new one is placed.
    # ONE pass to decide, then remove. Walking GetDrawings() a second time after
    # any Remove() raises "SwigPyObject is not iterable" -- the removal
    # invalidates the iterator, and the second loop is what pays for it.
    LABELS = ("3V3", "5V", "GND", "SDA", "SCL", "10", "11", "12", "13",
              "OLED", "RESET", "BOOT", "STATUS", "FAULT")
    doomed = []
    for d in list(board.GetDrawings()):
        cls = d.GetClass()
        if cls == "PCB_SHAPE" and d.GetLayer() == pcbnew.F_SilkS and \
                d.GetShape() in (pcbnew.SHAPE_T_CIRCLE, pcbnew.SHAPE_T_SEGMENT):
            doomed.append(d)            # a dot or cathode bar from a past run
        elif cls == "PCB_TEXT" and (d.GetText() in LABELS or d.GetText().startswith(
                ("ups-adaptor", "UPS", "PWR", "EXP", "GPL"))):
            doomed.append(d)
    for d in doomed:
        board.Remove(d)
    removed = len(doomed)

    # PHASE 3 -- add labels, using the captured boxes rather than live handles.
    def above(ref, text, h):
        box = boxes.get(ref)
        if not box:
            return 0
        x = (box[0] + box[2]) / 2.0
        y = box[1] - h * 0.9
        if y < T + 1.0:
            y = box[3] + h * 1.1
        add_text(board, text, x, y, pcbnew.F_SilkS, h)
        return 1

    def left_of(ref, text, h):
        box = boxes.get(ref)
        if not box:
            return 0
        x = max(L + h, box[0] - h * 0.9)
        y = (box[1] + box[3]) / 2.0
        add_text(board, text, x, y, pcbnew.F_SilkS, h, 90)
        return 1

    # A label that has to be adjacent to the thing it names, on a board with no
    # room to spare. `above()` puts text at a fixed offset and would happily
    # drop it on a neighbour; this searches outward from the part for the
    # nearest spot that clears every courtyard, every pad and every silk item
    # already placed, preferring the side given first.
    #
    # The buttons and the indicators need this more than anything else on the
    # board. SW1 and SW2 are identical black squares, and D3 and D4 are a
    # yellow and a red 0603 a millimetre apart: the reference designators say
    # which is which only if you have the schematic open, which is exactly when
    # you do not.
    # Phase 3 must not walk the board: removals above invalidated the SWIG
    # iterators, and GetDrawings() then raises "SwigPyObject is not iterable".
    # Everything here comes from geometry captured in phase 1, plus the labels
    # this phase adds as it goes.
    placed_labels = []
    occ = blockers + refboxes

    def beside(ref, text, h=1.0, prefer=("below", "above", "left", "right")):
        box = boxes.get(ref)
        if not box:
            return 0
        w = len(text) * h * 0.78          # KiCad stroke font, roughly
        cx = (box[0] + box[2]) / 2.0
        cy = (box[1] + box[3]) / 2.0
        GAP = 0.35

        def clear(x, y):
            bx = (x - w / 2 - GAP, y - h / 2 - GAP,
                  x + w / 2 + GAP, y + h / 2 + GAP)
            # Edge margin applies to the TEXT, not to its clearance halo: the
            # halo is for keeping off neighbours, and counting it against the
            # board edge cost ~0.7mm of usable strip on every side.
            if (x - w / 2 < L + 0.3 or x + w / 2 > R - 0.3 or
                    y - h / 2 < T + 0.3 or y + h / 2 > B - 0.3):
                return False
            return not any(not (bx[2] <= o[0] or bx[0] >= o[2] or
                                bx[3] <= o[1] or bx[1] >= o[3]) for o in occ)

        for side in prefer:
            for step in [d / 10.0 for d in range(6, 61)]:      # 0.6 .. 6.0 mm
                # Sideways offsets matter for above/below: STATUS and FAULT
                # sit over two 0603s a millimetre apart, and centring both on
                # their own part makes the words overlap, which pushed one of
                # them off to the far side of the board.
                for off in (0.0, 0.6, -0.6, 1.2, -1.2, 1.8, -1.8, 2.4, -2.4):
                    if side == "below":   x, y = cx + off, box[3] + h / 2 + step
                    elif side == "above": x, y = cx + off, box[1] - h / 2 - step
                    elif side == "left":  x, y = box[0] - w / 2 - step, cy + off
                    else:                 x, y = box[2] + w / 2 + step, cy + off
                    if clear(x, y):
                        add_text(board, text, x, y, pcbnew.F_SilkS, h)
                        occ.append((x - w / 2, y - h / 2, x + w / 2, y + h / 2))
                        placed_labels.append(text)
                        return 1
        print("  no room for the %r label next to %s" % (text, ref))
        return 0

    # A dot beside pin 1. KiCad's stock footprints DO carry an orientation mark,
    # but they put the unambiguous one on F.Fab -- which is documentation and is
    # never printed. What reaches the silkscreen is a notch or a chamfer in the
    # outline, a millimetre of 0.15mm line that is easy to miss on an assembled
    # board and easy to mistake for part of the outline.
    #
    # A filled dot next to pin 1 is the universal convention and is unmistakable.
    # For the LEDs, KiCad numbers the cathode pin 1, so the same dot marks the
    # cathode. It costs nothing and it is the difference between spotting a
    # part fitted backwards and powering it up.
    # Pads at their true outlines, plus the text already on the board. Note
    # this shares the `occ` list object by reference for everything added later.
    dot_occ = padrects + refboxes + placed_labels

    def evict(box):
        """Move the designator occupying `box` to the far side of its part."""
        ref = refowner.get(box)
        fp = board.FindFootprintByReference(ref) if ref else None
        if fp is None:
            return False
        c = fp.GetCourtyard(pcbnew.B_CrtYd if fp.IsFlipped() else pcbnew.F_CrtYd)
        if c.IsEmpty():
            return False
        cb = c.BBox()
        r = fp.Reference()
        mid = mm((cb.GetTop() + cb.GetBottom()) // 2)
        above = mm(r.GetPosition().y) < mid
        r.SetPosition(pcbnew.VECTOR2I(
            (cb.GetLeft() + cb.GetRight()) // 2,
            (cb.GetBottom() + pcbnew.FromMM(0.9)) if above
            else (cb.GetTop() - pcbnew.FromMM(0.9))))
        nb = r.GetBoundingBox()
        moved_box = (mm(nb.GetLeft()), mm(nb.GetTop()),
                     mm(nb.GetLeft()) + mm(nb.GetWidth()),
                     mm(nb.GetTop()) + mm(nb.GetHeight()))
        for lst in (occ, dot_occ):
            if box in lst:
                lst[lst.index(box)] = moved_box
        refowner[moved_box] = ref
        return True

    def polarity_dot(ref):
        """Put a filled dot outside the body, beside pin 1.

        The first position that merely fits is not good enough. A dot that ends
        up nearer a different pad than to the one it marks is worse than no dot:
        it reads as authoritative and points at the wrong end. So every angle at
        a given radius is scored on how much closer it is to pin 1 than to any
        other pad of the same part, and the clearest wins.

        Designators are soft obstacles. On a 0.95mm-pitch SOT-23-6 the set of
        unambiguous positions is a couple of square millimetres, and on this
        board U3's designator was sitting in the only one D2 had; the dot wins
        that argument and the designator moves.
        """
        got = pin1.get(ref)
        if not got:
            return 0
        px, py, psize = got
        box = bodies.get(ref) or boxes.get(ref)
        rad = 0.25                    # 0.5mm dot: visible, not obtrusive
        rest = others.get(ref, [])
        MARGIN = 0.5                  # mm of unambiguity, not a tie-break

        def search(soft_ok):
            for step in [d / 10.0 for d in range(3, 26)]:
                best = None
                for k in range(36):
                    a = 2 * math.pi * k / 36.0
                    x = px + (psize / 2 + rad + step) * math.cos(a)
                    y = py + (psize / 2 + rad + step) * math.sin(a)
                    # outside the part, on free board, inside the edge
                    if box and (box[0] - rad < x < box[2] + rad and
                                box[1] - rad < y < box[3] + rad):
                        continue
                    bx = (x - rad - 0.25, y - rad - 0.25,
                          x + rad + 0.25, y + rad + 0.25)
                    if (bx[0] < L + 0.3 or bx[2] > R - 0.3
                            or bx[1] < T + 0.3 or bx[3] > B - 0.3):
                        continue
                    hit = [o for o in dot_occ
                           if not (bx[2] <= o[0] or bx[0] >= o[2] or
                                   bx[3] <= o[1] or bx[1] >= o[3])]
                    if hit and not (soft_ok and all(o in refowner for o in hit)):
                        continue
                    d1 = math.hypot(x - px, y - py)
                    dn = min((math.hypot(x - ox, y - oy) for ox, oy in rest),
                             default=d1 + 99.0)
                    if dn - d1 < MARGIN:
                        continue
                    if best is None or dn - d1 > best[0]:
                        best = (dn - d1, x, y, bx, hit)
                if best is not None:
                    return best
            return None

        found = search(False) or search(True)
        if found is None:
            print("  no room for an unambiguous polarity dot on %s" % ref)
            return 0
        _, x, y, bx, hit = found
        for o in hit:
            evict(o)
        c = pcbnew.PCB_SHAPE(board)
        c.SetLayer(pcbnew.F_SilkS)
        if ref in CATHODE_BAR:
            # A bar ACROSS the cathode end: perpendicular to the line from the
            # body out to the marked pad, so it reads as a diode's own stripe.
            bcx = (box[0] + box[2]) / 2.0 if box else px
            bcy = (box[1] + box[3]) / 2.0 if box else py
            vx, vy = x - bcx, y - bcy
            n = math.hypot(vx, vy) or 1.0
            hx, hy = -vy / n * 0.5, vx / n * 0.5      # 1.0mm bar, half each way
            c.SetShape(pcbnew.SHAPE_T_SEGMENT)
            c.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x - hx), pcbnew.FromMM(y - hy)))
            c.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x + hx), pcbnew.FromMM(y + hy)))
            c.SetWidth(pcbnew.FromMM(0.25))
        else:
            c.SetShape(pcbnew.SHAPE_T_CIRCLE)
            c.SetCenter(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
            c.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x + rad), pcbnew.FromMM(y)))
            c.SetFilled(True)
            c.SetWidth(pcbnew.FromMM(0.15))
        board.Add(c)
        occ.append(bx)
        dot_occ.append(bx)
        return 1

    dots = sum(polarity_dot(r) for r in sorted(POLARISED))
    print("  marked %d of %d polarised parts with a pin-1 dot"
          % (dots, len(POLARISED)))

    labels = 0
    labels += beside("SW1", "RESET", 1.0, ("below", "above", "left", "right"))
    labels += beside("SW2", "BOOT", 1.0, ("below", "above", "right", "left"))
    # Above each indicator, offset left and right so the two words sit over
    # their own LED rather than meeting in the 1mm gap between them.
    labels += beside("D3", "STATUS", 1.0, ("above", "left", "below", "right"))
    labels += beside("D4", "FAULT", 1.0, ("above", "right", "below", "left"))
    labels += above("J2", "UPS", 1.2)          # host port: the board feeds this
    labels += above("J1", "PWR/PROG", 1.0)     # power, programming, console
    labels += left_of("J5", "EXP", 0.9)
    labels += above("J3", "OLED", 0.9)

    # Pin labels, placed to whichever side of the header has board left.
    pinlabels = 0
    for ref, plist in pins.items():
        box = boxes.get(ref)
        if not box:
            continue
        to_right = (L + R) / 2 - box[0] > 0      # header on the left half?
        for num, net, px, py in plist:
            txt = PIN_TEXT.get(net)
            if not txt:
                continue
            # 0.95mm, not 0.55: the label is 1.0mm tall once silk-jlc.py raises
            # it to JLC's minimum, and at 0.55 it overlapped the header's pads.
            x = box[2] + 0.95 if to_right else box[0] - 0.95
            anchor_right = not to_right
            t = add_text(board, txt, x, py, pcbnew.F_SilkS, 0.7)
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT if anchor_right
                              else pcbnew.GR_TEXT_H_ALIGN_LEFT)
            pinlabels += 1
    labels += pinlabels

    add_text(board, "ups-adaptor Rev A", (L + R) / 2, T + 6.0, pcbnew.B_SilkS, 1.2)
    add_text(board, "GPL-3.0-or-later", (L + R) / 2, T + 8.2, pcbnew.B_SilkS, 0.9)
    labels += 2

    pcbnew.SaveBoard(PCB, board)
    print("hid %d value fields and %d passive/button designators, moved %d "
          "references clear of their pads%s"
          % (hidden, dropped, moved,
             ", %d out of label strips" % nudged_out if nudged_out else ""))
    print("added %d silkscreen labels including %d header pin names "
          "(removed %d from a previous run)" % (labels, pinlabels, removed))
    print("NOTE each dot marks pin 1. D3 and D4 get a BAR across the cathode "
          "instead,")
    print("     because a dot on a diode reads as the capacitor convention and "
          "means the")
    print("     opposite. Stock footprint silk is left alone either way.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
