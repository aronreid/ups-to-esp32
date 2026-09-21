#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Place every part on the Rev A board, using KiCad's own pcbnew API.

WHY pcbnew AND NOT HAND-WRITTEN S-EXPRESSIONS
Putting a part on the bottom layer means mirroring its pads, silkscreen and
courtyard. Getting that wrong produces a file that loads, passes DRC, and is
wrong -- every bottom part mirrored, every pad in the wrong place, discovered
after assembly. pcbnew.Flip() is KiCad's own implementation, so it is correct by
construction. That is worth more than avoiding a dependency.

THE SPLIT
Top:    the module, both USB connectors, headers, buttons, CH340C, the LDO and
        both ESD arrays. The ESD arrays stay on top deliberately -- routing them
        through vias adds inductance exactly where a TVS needs none.
Bottom: passives, the load switch and the auto-reset transistors. That leaves the
        bottom layer nearly empty, so its ground pour stays continuous under the
        USB pairs, which is what a 12 Mbps pair actually needs.

Run with KiCad's Python:
    KPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
    $KPY tools/place-pcb.py
"""
import os
import sys

import pcbnew

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

MM = pcbnew.FromMM
TOMM = pcbnew.ToMM

# Space between courtyards. 0.3mm was the original value and it is why the board
# could not be routed: it is enough for parts not to touch and nothing else. A
# 0.25mm track needs its own width plus clearance on both sides to pass between
# two parts, so anything under about 0.8mm leaves no channels at all and the
# router is left threading around a solid block of components.
CLEARANCE = 1.2
GRID = 0.5
EDGE = 0.6
ANTENNA_STRIP = 2.5       # top strip kept clear of the antenna's near field
SPREAD = 0.8              # weight pushing parts apart, against net attraction

# Anchors, in board-local mm from the board's top-left corner.
ANCHORS = {
    "U1": (18.0, 6.5, 0),      # module: topmost pad 0.8mm in, antenna overhangs
    # Both connectors on the bottom edge, spaced in even thirds: about 1.5mm of
    # gap at the left edge, between them, and at the right. They were at 15.5
    # and 30.0, which left 2.62mm on the left and 0.74mm on the right --
    # visibly lopsided, and 0.74mm is tight against the edge for a part that
    # gets a cable pulled out of it.
    # The Y values put each connector's BODY FRONT 0.5mm past the board edge
    # (58.0): body front is +7.10 from the XKB's origin and +3.65 from the
    # HRO's. They used to sit at 49.60 and 53.05, which left both mouths
    # recessed 1.3mm INSIDE the edge. A plug's overmold is taller than the
    # receptacle and hangs below the PCB surface, so it would have struck the
    # board edge and stopped 1.3mm short of seated -- marginal on USB-A, and on
    # USB-C, with ~2mm of contact wipe, the port that powers and programs the
    # board. Nearest copper is still 2.3mm (C) and 5.4mm (A) from the edge.
    "J2": (11.04, 51.40, 0),    # USB-A to the UPS
    "J1": (27.39, 54.85, 0),    # USB-C: power, programming, console
    "J5": (2.5, 18.5, 0),      # expansion header, left edge (male: jumper
                               # wires have female ends)
    # J3 sits low on purpose. A 0.96" module is about 27mm square and extends
    # perpendicular to the socket, centred on it -- with J3 up at y=11.8 the
    # module overhung the top edge by 2.1mm, putting PCB and its ground plane
    # inside the antenna's exclusion. At y=24 it lands wholly on the board.
    "J3": (31.4, 24.0, 0),
    # Buttons sit as high as the edge margin allows, clear of the module's pads
    # and easy to reach past an enclosure wall.
    "SW1": (4.6, 5.5, 90),     # RESET
    "SW2": (31.1, 5.5, 90),    # BOOT
    # Status LEDs on one line. They are read together, so they should sit
    # together; a 1.5mm stagger just looks like a mistake.
    "D3": (16.0, 30.0, 0),     # green, status
    "D4": (20.0, 30.0, 0),     # red, fault
}

# Stay on top: needs physical access, must be SEEN, is large, or must not sit
# behind a via.
#
# D3 and D4 are the status LEDs and were originally flipped to the bottom with
# the other passives, which put the only local indicators on the face nobody
# looks at. An indicator on the underside of a board in a closet indicates
# nothing.
TOP_ONLY = {"U1", "J1", "J2", "J3", "J4", "J5", "SW1", "SW2", "U2", "U3",
            "D1", "D2", "D3", "D4"}

# EVERYTHING goes on top. JLCPCB's Economic PCBA is "Single sided placement"
# only; double-sided needs Standard PCBA, whose minimum board is 70 x 70 mm --
# this one is 36 x 58 -- and whose fees are several times higher. A bare
# two-layer PCB costs the same with parts on one side or two, which is what the
# earlier layout was reasoning from, but ASSEMBLY does not. Rev A shipped its
# fab files with 25 parts on the bottom and could not have been ordered on the
# service this project is designed around. Both copper layers are still used
# for routing; the bottom simply carries no parts, which also leaves it a
# nearly unbroken ground plane under the USB pairs.
SINGLE_SIDED = True

# A decoupling capacitor belongs beside the pin it decouples, not beside the
# regulator. The placer pulls each part toward the centroid of everything it
# shares a net with, and since +3V3 and GND reach nearly every part, all five
# 3V3 capacitors clustered at the regulator -- the nearest one was 25 mm from
# the ESP32's supply pin, down a 0.2 mm track. Wi-Fi transmit bursts step the
# module's current by ~300 mA in microseconds; that is what the bulk capacitor
# at the pin is for. These parts are placed right after their target, pulled
# to that one PAD, with the spreading term off.
PIN_TARGETS = {
    "C5": ("U1", "2"),     # 22uF bulk at the module's 3V3 pin
    "C9": ("U1", "2"),     # 100nF beside it
    "C1": ("U1", "3"),     # EN's RC capacitor
    "C3": ("U2", "16"),    # CH340C VCC
    "C6": ("U2", "4"),     # CH340C V3
    "C4": ("U3", "3"),     # regulator input
    "C2": ("U3", "2"),     # regulator output: the AMS1117 needs it close
    "C8": ("U4", "1"),     # load switch output, HF
    "C7": ("U4", "1"),     # load switch output, bulk
    # Protection goes AT the connector it protects. D2 once landed 30mm from
    # J2, beside the OLED socket, so a strike on the UPS cable crossed half the
    # board -- past the regulator and the module -- before anything clamped it,
    # and the USB pair ran 69mm on a 58mm board to get there and back.
    "D2": ("J2", "2"),     # ESD array on the UPS port
    "D1": ("J1", "A6"),    # ESD array on the USB-C port
    "U4": ("J2", "1"),     # load switch beside the VBUS pin it feeds
}

# Strips kept free of parts so the header pin names have somewhere to be. With
# everything on one side the passives moved in beside the headers, and the
# labels ended up clipped to "3" and "A". Board-local mm: x0, y0, x1, y1.
LABEL_RESERVES = [
    (4.4, 16.8, 7.6, 35.4),      # right of J5: 3V3 5V GND 10 11 12 13
    (26.2, 22.3, 29.4, 33.3),    # left of J3:  3V3 GND SDA SCL
    # The two buttons are identical black squares and the two indicators are a
    # yellow and a red 0603 a millimetre apart. Their reference designators say
    # which is which only if you have the schematic open, which is exactly when
    # you do not. Without these strips the placer fills the space and the label
    # search ends up putting BOOT 8mm from its button, which reads as belonging
    # to whatever it landed next to.
    # Sized to the word, not rounded up: this strip sits between SW1 and the
    # module's decoupling, and every spare millimetre of it pushes those
    # capacitors further from the pin they serve.
    (2.2, 9.6, 7.0, 11.3),       # below SW1: RESET
    (29.4, 9.6, 32.8, 11.3),     # below SW2: BOOT
    (12.6, 26.6, 23.4, 28.7),    # above D3/D4: STATUS and FAULT
]


def board_rect(board):
    bb = board.GetBoardEdgesBoundingBox()
    return (TOMM(bb.GetLeft()), TOMM(bb.GetTop()),
            TOMM(bb.GetRight()), TOMM(bb.GetBottom()))


def rel_box(fp, rot):
    """Courtyard box relative to the footprint origin at a given rotation."""
    keep_pos, keep_rot = fp.GetPosition(), fp.GetOrientationDegrees()
    fp.SetOrientationDegrees(rot)
    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    layer = pcbnew.B_CrtYd if fp.IsFlipped() else pcbnew.F_CrtYd
    cy = fp.GetCourtyard(layer)
    if cy.IsEmpty():
        bb = fp.GetBoundingBox(False, False)
        box = (TOMM(bb.GetLeft()), TOMM(bb.GetTop()),
               TOMM(bb.GetRight()), TOMM(bb.GetBottom()))
    else:
        bb = cy.BBox()
        box = (TOMM(bb.GetLeft()), TOMM(bb.GetTop()),
               TOMM(bb.GetRight()), TOMM(bb.GetBottom()))
    # Union with the pads. A courtyard does not always enclose its own pads --
    # U1 and U3 collided by 0.012mm because of exactly that.
    pb = pad_box(fp)
    if pb:
        box = (min(box[0], pb[0]), min(box[1], pb[1]),
               max(box[2], pb[2]), max(box[3], pb[3]))
    fp.SetPosition(keep_pos)
    fp.SetOrientationDegrees(keep_rot)
    return box


def has_through_hole(fp):
    """A through-hole part consumes BOTH sides: its holes pass through the
    board, so a bottom-side part cannot sit under a top-side header."""
    for pad in fp.Pads():
        if pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
            return True
    return False


def pad_box(fp):
    """Bounding box of a footprint's pads, in mm, absolute."""
    xs, ys = [], []
    for pad in fp.Pads():
        bb = pad.GetBoundingBox()
        xs += [TOMM(bb.GetLeft()), TOMM(bb.GetRight())]
        ys += [TOMM(bb.GetTop()), TOMM(bb.GetBottom())]
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


def overlaps(a, b):
    return (min(a[2], b[2]) - max(a[0], b[0]) > 0 and
            min(a[3], b[3]) - max(a[1], b[1]) > 0)


def main():
    board = pcbnew.LoadBoard(PCB)
    x0, y0, x1, y1 = board_rect(board)
    W, H = x1 - x0, y1 - y0
    print("board %.1f x %.1f mm at (%.1f, %.1f)" % (W, H, x0, y0))

    fps = {fp.GetReference(): fp for fp in board.GetFootprints()}

    # Nets each part touches, ignoring the rails so the cost function is driven
    # by signals rather than by everything sharing ground.
    RAILS = {"GND", "+3V3", "+5V", ""}
    nets_of = {}
    for ref, fp in fps.items():
        s = set()
        for pad in fp.Pads():
            n = pad.GetNetname()
            if n not in RAILS:
                s.add(n)
        nets_of[ref] = s

    # Flip the bottom-side parts first, so their courtyards come from B.CrtYd.
    flipped = []
    for ref, fp in fps.items():
        if not SINGLE_SIDED and ref not in TOP_ONLY:
            fp.Flip(fp.GetPosition(), False)
            flipped.append(ref)
    print("flipped to bottom: %d parts" % len(flipped))

    occupied_top, occupied_bot = [], []

    # The module blocks the top. Use the union of its body and its PADS, not the
    # courtyard: the courtyard is the 15mm antenna keepout, a copper rule rather
    # than a placement one, but the pads reach 0.5mm wider than the can and a
    # part butted against the body alone lands on them.
    ax, ay, arot = ANCHORS["U1"]
    fpu1 = fps["U1"]
    fpu1.SetOrientationDegrees(arot)
    fpu1.SetPosition(pcbnew.VECTOR2I(MM(x0 + ax), MM(y0 + ay)))
    pb = pad_box(fpu1)
    body = (min(x0 + ax - 9.0, pb[0]) - CLEARANCE,
            min(y0 + ay - 12.75, pb[1]) - CLEARANCE,
            max(x0 + ax + 9.0, pb[2]) + CLEARANCE,
            max(y0 + ay + 12.75, pb[3]) + CLEARANCE)

    placed_box = {}
    for ref, (lx, ly, rot) in ANCHORS.items():
        fp = fps[ref]
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(pcbnew.VECTOR2I(MM(x0 + lx), MM(y0 + ly)))
        rb = rel_box(fp, rot)
        box = (x0 + lx + rb[0], y0 + ly + rb[1], x0 + lx + rb[2], y0 + ly + rb[3])
        placed_box[ref] = box
        blk = body if ref == "U1" else box
        occupied_top.append(blk)
        if ref == "U1" or has_through_hole(fp):
            occupied_bot.append(blk)

    for lx0, ly0, lx1, ly1 in LABEL_RESERVES:
        occupied_top.append((x0 + lx0, y0 + ly0, x0 + lx1, y0 + ly1))

    todo = []
    for ref, fp in fps.items():
        if ref in ANCHORS:
            continue
        rb = rel_box(fp, 0)
        todo.append(((rb[2] - rb[0]) * (rb[3] - rb[1]), ref))
    todo.sort(reverse=True)

    # Big parts first, as before -- but each pin-targeted part goes in
    # immediately after the part it serves, before anything else can take the
    # ground beside that pin.
    by_target = {}
    for ref, (tref, _pad) in PIN_TARGETS.items():
        if ref in fps and tref in fps:
            by_target.setdefault(tref, []).append(ref)
    order = []

    def emit(ref):
        # Recursive: U4 follows J2, and C7/C8 follow U4.
        order.append(ref)
        for dep in sorted(by_target.get(ref, [])):
            emit(dep)

    # The large free parts claim their ground first. Putting the small pinned
    # parts in ahead of them left nowhere for the SOT-223 regulator to go.
    BIG = 30.0      # mm2 of courtyard: the SOIC-16 and the SOT-223
    pinned = lambda r: r in PIN_TARGETS and PIN_TARGETS[r][0] in fps
    for area_, ref in todo:
        if area_ >= BIG and not pinned(ref):
            emit(ref)
    for tref in ANCHORS:
        for dep in sorted(by_target.get(tref, [])):
            emit(dep)
    for area_, ref in todo:
        if area_ < BIG and not pinned(ref):
            emit(ref)

    def pad_xy(ref, num):
        for pad in fps[ref].Pads():
            if pad.GetNumber() == num:
                pos = pad.GetPosition()
                return TOMM(pos.x), TOMM(pos.y)
        return None

    failed = []
    for ref in order:
        fp = fps[ref]
        bottom = (not SINGLE_SIDED) and ref not in TOP_ONLY
        occ = occupied_bot if bottom else occupied_top
        tht = has_through_hole(fp)
        other = occupied_top if bottom else occupied_bot
        # On the bottom, the module's body is irrelevant; only its pads matter,
        # and those are through the board, so keep clear of the body anyway.
        extra = [] if not bottom else [body]

        mine = nets_of.get(ref, set())
        targets = [((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
                   for o, b in placed_box.items() if mine & nets_of.get(o, set())]
        if targets:
            tx = sum(t[0] for t in targets) / len(targets)
            ty = sum(t[1] for t in targets) / len(targets)
        else:
            tx, ty = x0 + W / 2, y0 + H / 2
        spread = SPREAD
        if ref in PIN_TARGETS:
            pin = pad_xy(*PIN_TARGETS[ref])
            if pin:
                tx, ty = pin
                spread = 0.0

        best = None
        for rot in (0, 90):
            rb = rel_box(fp, rot)
            px = x0 + EDGE - rb[0]
            while px + rb[2] <= x1 - EDGE + 1e-9:
                py = y0 + max(EDGE, ANTENNA_STRIP) - rb[1]
                while py + rb[3] <= y1 - EDGE + 1e-9:
                    box = (px + rb[0] - CLEARANCE / 2, py + rb[1] - CLEARANCE / 2,
                           px + rb[2] + CLEARANCE / 2, py + rb[3] + CLEARANCE / 2)
                    if not any(overlaps(box, b) for b in occ) and \
                       not any(overlaps(box, b) for b in extra) and \
                       not (tht and any(overlaps(box, b) for b in other)):
                        # Pull toward the parts this one connects to, but push
                        # away from whatever is already nearest. Attraction alone
                        # packs everything onto the centroid and leaves the rest
                        # of the board empty, which is exactly what happened.
                        attract = ((px - tx) ** 2 + (py - ty) ** 2) ** 0.5
                        near = min(((((box[0] + box[2]) / 2 - (b[0] + b[2]) / 2) ** 2 +
                                     ((box[1] + box[3]) / 2 - (b[1] + b[3]) / 2) ** 2) ** 0.5)
                                   for b in occ) if occ else 99.0
                        cost = attract - spread * min(near, 12.0)
                        if best is None or cost < best[0]:
                            best = (cost, px, py, rot,
                                    (px + rb[0], py + rb[1], px + rb[2], py + rb[3]))
                    py += GRID
                px += GRID
        if best is None:
            failed.append(ref)
            continue
        _c, px, py, rot, box = best
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(pcbnew.VECTOR2I(MM(px), MM(py)))
        placed_box[ref] = box
        occ.append(box)
        if has_through_hole(fp):
            (occupied_top if bottom else occupied_bot).append(box)

    if failed:
        print("FAILED to place: %s" % ", ".join(sorted(failed)))

    area = W * H
    ta = sum((b[2] - b[0]) * (b[3] - b[1]) for b in occupied_top)
    ba = sum((b[2] - b[0]) * (b[3] - b[1]) for b in occupied_bot)
    print("top   %2d parts, %3.0f%% of board area" % (len(occupied_top), 100 * ta / area))
    print("bottom %2d parts, %3.0f%% of board area" % (len(occupied_bot), 100 * ba / area))

    board.BuildListOfNets()
    pcbnew.SaveBoard(PCB, board)
    print("saved %s" % PCB)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
