#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stitch the two ground pours together with vias.

Pours on opposite layers are not connected just because they share a net name.
Without stitching they are two separate sheets of copper, and DRC says so. It
also matters electrically: the return current under a USB pair has to be able to
get back, and a top-layer pour that cannot reach the bottom plane is not a
reference plane at all.

Vias go on a grid, skipping anywhere they would foul a pad, a track or the board
edge. Run after routing and before the final pour fill.
"""
import importlib.util
import math
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.join(os.path.dirname(HERE), "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

# The floating-pad question is "is this pad in the same electrical network as the
# main plane?", which needs the whole GND graph -- pads, tracks, vias and pour
# islands. Asking the cheap version instead ("is the pad centre inside the main
# plane polygon?") makes this script disagree with test_ground.py: it re-rescued
# U2.1, C1.2 and C4.2 on every pass, because their rescue via already tied them
# in and only the polygon test could not see it, while the pads that really were
# floating went unattended. One analysis, used by both.
_tg_spec = importlib.util.spec_from_file_location(
    "test_ground", os.path.join(HERE, "test", "test_ground.py"))
_test_ground = importlib.util.module_from_spec(_tg_spec)
_tg_spec.loader.exec_module(_test_ground)

PITCH = 1.8        # mm between stitching vias
DRILL = 0.3        # JLC standard 2-layer minimum
WIDTH = 0.6        # 0.15mm annulus
# Clearance from other copper. 0.45mm was starving the grid pass -- it reported
# zero vias placed on a densely routed board, which is why the pours stayed in
# fragments and GND pads ended up floating. The design rule is 0.2mm; 0.28 keeps
# a margin over it while actually fitting.
KEEP = 0.38        # clearance from other copper
EDGE = 1.2         # keep off the board edge
ANTENNA_STRIP = 3.0


# --- true geometry, for the floating-pad rescue -----------------------------
#
# The grid pass models every obstacle as its bounding box, which is fine for
# pads and vias but badly wrong for a long diagonal track: its box is the whole
# rectangle it spans, so it vetoes ground it never touches. That is why the
# rescue could not find a single legal via beside C1 pin 2 or D1 pin 2 -- both
# sit in a pocket crossed by diagonal signal traces. These helpers measure real
# distance to the track centreline instead.

RESCUE_CLR = 0.22      # design rule is 0.2mm; a hair over it
RESCUE_HOLE = 0.5      # hole-to-hole rule, edge to edge


def _point_seg_mm(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    l2 = vx * vx + vy * vy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - x1) * vx + (py - y1) * vy) / l2))
    return ((px - (x1 + t * vx)) ** 2 + (py - (y1 + t * vy)) ** 2) ** 0.5


def _seg_seg_mm(ax, ay, bx, by, cx, cy, dx, dy):
    """Distance between two segment centrelines.

    The four endpoint-to-segment distances are NOT enough on their own: two
    segments crossing in an X have every endpoint well clear of the other
    segment, so the minimum comes out large and the crossing goes unseen. That
    is exactly what happened -- the rescue placed three stubs straight across
    +3V3, +5V and PROG_DM. Test for a proper intersection first."""
    def side(x1, y1, x2, y2, px, py):
        return (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
    d1 = side(ax, ay, bx, by, cx, cy)
    d2 = side(ax, ay, bx, by, dx, dy)
    d3 = side(cx, cy, dx, dy, ax, ay)
    d4 = side(cx, cy, dx, dy, bx, by)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(_point_seg_mm(ax, ay, cx, cy, dx, dy),
               _point_seg_mm(bx, by, cx, cy, dx, dy),
               _point_seg_mm(cx, cy, ax, ay, bx, by),
               _point_seg_mm(dx, dy, ax, ay, bx, by))


def _collect_geometry(board):
    """Obstacles as real shapes: tracks as segments, pads and vias as boxes."""
    mm = pcbnew.ToMM
    segs, boxes, drills = [], [], []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            layers = tuple(L for L in pad.GetLayerSet().Seq()
                           if L in (pcbnew.F_Cu, pcbnew.B_Cu))
            if not layers:
                continue
            bb = pad.GetBoundingBox()
            boxes.append((mm(bb.GetLeft()), mm(bb.GetTop()),
                          mm(bb.GetRight()), mm(bb.GetBottom()),
                          layers, pad.GetNetname()))
            if pad.GetDrillSizeX() > 0:
                pos = pad.GetPosition()
                drills.append((mm(pos.x), mm(pos.y), mm(pad.GetDrillSizeX())))
    for t in board.GetTracks():
        net = t.GetNetname()
        if isinstance(t, pcbnew.PCB_VIA):
            pos = t.GetPosition()
            drills.append((mm(pos.x), mm(pos.y), mm(t.GetDrillValue())))
            segs.append((mm(pos.x), mm(pos.y), mm(pos.x), mm(pos.y),
                         mm(t.GetWidth()) / 2.0, None, net))
        else:
            s, e = t.GetStart(), t.GetEnd()
            segs.append((mm(s.x), mm(s.y), mm(e.x), mm(e.y),
                         mm(t.GetWidth()) / 2.0, t.GetLayer(), net))
    return segs, boxes, drills


def _near(geom, px, py, radius):
    """The obstacles within `radius` mm of a point, as a smaller geom tuple.

    Scanning all ~900 obstacles for each of the several thousand candidate via
    positions made the rescue take longer than the rest of the build put
    together. Nothing more than a few millimetres away can matter, so slice the
    lists once per pad and search that."""
    segs, boxes, drills = geom
    r = radius
    ns = [g for g in segs
          if min(g[0], g[2]) - g[4] - r <= px <= max(g[0], g[2]) + g[4] + r
          and min(g[1], g[3]) - g[4] - r <= py <= max(g[1], g[3]) + g[4] + r]
    nb = [b for b in boxes
          if b[0] - r <= px <= b[2] + r and b[1] - r <= py <= b[3] + r]
    nd = [d for d in drills
          if abs(d[0] - px) <= r and abs(d[1] - py) <= r]
    return (ns, nb, nd)


def _clear_of(geom, ax, ay, bx, by, half, layers, own_net="GND", clr=None):
    """True if a segment of half-width `half` on `layers` clears every obstacle
    not on `own_net` by the design rule."""
    segs, boxes, _ = geom
    global RESCUE_CLR
    keep = RESCUE_CLR if clr is None else clr
    for x1, y1, x2, y2, ohalf, layer, net in segs:
        if net == own_net or (layer is not None and layer not in layers):
            continue
        if _seg_seg_mm(ax, ay, bx, by, x1, y1, x2, y2) < half + ohalf + keep:
            return False
    pad = half + keep
    for x0, y0, x1, y1, blayers, net in boxes:
        if net == own_net or not set(blayers) & set(layers):
            continue
        ex0, ey0, ex1, ey1 = x0 - pad, y0 - pad, x1 + pad, y1 + pad
        if (ex0 <= ax <= ex1 and ey0 <= ay <= ey1) or (ex0 <= bx <= ex1 and ey0 <= by <= ey1):
            return False
        for X1, Y1, X2, Y2 in ((ex0, ey0, ex1, ey0), (ex1, ey0, ex1, ey1),
                               (ex1, ey1, ex0, ey1), (ex0, ey1, ex0, ey0)):
            if _seg_seg_mm(ax, ay, bx, by, X1, Y1, X2, Y2) <= 0.0:
                return False
    return True




def _bridge_round(board, obstacles, fits, gnd):
    """One pass: tie every island that is not the main plane on its layer to the
    opposite layer's main plane, with a via inside both.

    Run repeatedly -- filling changes which islands exist, so a fragment that had
    no route to the plane on one pass may have one on the next."""
    def islands(zone, layer):
        polys = zone.GetFilledPolysList(layer)
        out = []
        for i in range(polys.OutlineCount()):
            one = pcbnew.SHAPE_POLY_SET()
            one.AddOutline(polys.Outline(i))
            out.append((one.Area(), one))
        out.sort(key=lambda t: -t[0])
        return out

    by_layer = {}
    for zone in board.Zones():
        if zone.GetNetname() != "GND":
            continue
        for layer in zone.GetLayerSet().Seq():
            by_layer.setdefault(layer, []).extend(islands(zone, layer))
    for layer in by_layer:
        by_layer[layer].sort(key=lambda t: -t[0])

    added = 0
    for layer, isl in by_layer.items():
        others = [l for l in by_layer if l != layer]
        if not others or len(isl) < 2:
            continue
        # Target ANY larger island on the other layer, not only the main plane.
        # A fragment often has no overlap with the plane itself but plenty with a
        # mid-sized region that does; bridging to that merges it upward, and
        # repeating the round carries the merge the rest of the way.
        targets = [(a, poly) for a, poly in by_layer[others[0]]]
        if not targets:
            continue
        for _area, one in isl[1:]:
            bigger = [poly for a, poly in targets if a > _area]
            if not bigger:
                bigger = [targets[0][1]]
            ibb = one.BBox()
            pt = None
            cands = [pcbnew.VECTOR2I((ibb.GetLeft() + ibb.GetRight()) // 2,
                                     (ibb.GetTop() + ibb.GetBottom()) // 2)]
            for fx in range(1, 24):
                for fy in range(1, 24):
                    cands.append(pcbnew.VECTOR2I(
                        ibb.GetLeft() + ibb.GetWidth() * fx // 24,
                        ibb.GetTop() + ibb.GetHeight() * fy // 24))
            for cand in cands:
                if not one.Contains(cand) or not fits(cand.x, cand.y):
                    continue
                if any(t.Contains(cand) for t in bigger):
                    pt = cand
                    break
            if pt is None:
                continue
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pt)
            via.SetDrill(pcbnew.FromMM(DRILL))
            via.SetWidth(pcbnew.FromMM(WIDTH))
            via.SetViaType(pcbnew.VIATYPE_THROUGH)
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            via.SetNetCode(gnd.GetNetCode())
            board.Add(via)
            obstacles.append((via.GetBoundingBox(), "GND"))
            added += 1
    return added


def main():
    board = pcbnew.LoadBoard(PCB)
    gnd = board.FindNet("GND")
    if gnd is None:
        print("no GND net")
        return 1

    # No de-duplication pass here. Removing tracks through the SWIG bindings
    # invalidates the footprint iterator that runs next, which surfaces as
    # "SwigPyObject has no attribute Pads" several lines later. The pipeline
    # always regenerates the board first, so there is never stale stitching to
    # remove; running this script twice by hand would double up.

    # Freerouting works to the board outline and knows nothing about the edge
    # clearance rule, so it will happily drop a via a fifth of a millimetre from
    # the edge. Push any offender inward before stitching adds more.
    EDGE_RULE = 0.3
    nudged = 0
    ebb = board.GetBoardEdgesBoundingBox()
    eL, eT = pcbnew.ToMM(ebb.GetLeft()), pcbnew.ToMM(ebb.GetTop())
    eR, eB = pcbnew.ToMM(ebb.GetRight()), pcbnew.ToMM(ebb.GetBottom())
    for t in board.GetTracks():
        if not isinstance(t, pcbnew.PCB_VIA):
            continue
        pos = t.GetPosition()
        vx, vy = pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
        need = pcbnew.ToMM(t.GetWidth()) / 2.0 + EDGE_RULE
        nx = min(max(vx, eL + need), eR - need)
        ny = min(max(vy, eT + need), eB - need)
        if abs(nx - vx) > 1e-6 or abs(ny - vy) > 1e-6:
            t.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(nx), pcbnew.FromMM(ny)))
            nudged += 1
    if nudged:
        print("nudged %d vias away from the board edge" % nudged)

    obstacles = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            bb = pad.GetBoundingBox()
            obstacles.append((bb, pad.GetNetname()))
    for t in board.GetTracks():
        bb = t.GetBoundingBox()
        obstacles.append((bb, t.GetNetname()))

    bb = board.GetBoardEdgesBoundingBox()
    L, T = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
    R, B = pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())

    half = WIDTH / 2.0 + KEEP

    HOLE_TO_HOLE = 0.55      # rule is 0.5mm; keep a margin
    PAD_HOLE_TO_HOLE = 0.50  # JLC wants 0.45mm between plated holes

    # Every drilled hole on the board, vias and component holes alike. Checking
    # only vias let the grid put a via 0.32mm from J3's header hole: JLC wants
    # 0.45mm between plated holes and that is a drill-breakage limit, so it does
    # not care that one hole belongs to a via and the other to a connector.
    pad_holes = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetDrillSizeX() > 0:
                pos = pad.GetPosition()
                pad_holes.append((pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y),
                                  pcbnew.ToMM(pad.GetDrillSizeX())))

    def fits(px_iu, py_iu):
        """True if a via centred here clears everything that is not GND.

        Same-net copper may touch, but two DRILLS may not come within the
        hole-to-hole rule however the nets relate -- that is a fabrication limit,
        not an electrical one, and ignoring it produced six hole_clearance
        violations once the via grid got dense."""
        x_mm, y_mm = pcbnew.ToMM(px_iu), pcbnew.ToMM(py_iu)
        for hx, hy, hd in pad_holes:
            d = ((hx - x_mm) ** 2 + (hy - y_mm) ** 2) ** 0.5
            if d < (DRILL + hd) / 2.0 + PAD_HOLE_TO_HOLE:
                return False
        for t in board.GetTracks():
            if not isinstance(t, pcbnew.PCB_VIA):
                continue
            d = ((pcbnew.ToMM(t.GetPosition().x - px_iu)) ** 2 +
                 (pcbnew.ToMM(t.GetPosition().y - py_iu)) ** 2) ** 0.5
            if d < DRILL + HOLE_TO_HOLE:
                return False
        box = pcbnew.BOX2I(
            pcbnew.VECTOR2I(px_iu - pcbnew.FromMM(half), py_iu - pcbnew.FromMM(half)),
            pcbnew.VECTOR2I(pcbnew.FromMM(2 * half), pcbnew.FromMM(2 * half)))
        for obb, net in obstacles:
            if not box.Intersects(obb):
                continue
            if net != "GND":
                return False
            if obb.Contains(pcbnew.VECTOR2I(px_iu, py_iu)):
                return False
        return True

    added = 0
    y = T + EDGE
    while y <= B - EDGE:
        x = L + EDGE
        while x <= R - EDGE:
            if y - T < ANTENNA_STRIP:
                x += PITCH
                continue
            if fits(pcbnew.FromMM(x), pcbnew.FromMM(y)):
                via = pcbnew.PCB_VIA(board)
                via.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
                via.SetDrill(pcbnew.FromMM(DRILL))
                via.SetWidth(pcbnew.FromMM(WIDTH))
                via.SetViaType(pcbnew.VIATYPE_THROUGH)
                via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                via.SetNetCode(gnd.GetNetCode())
                board.Add(via)
                obstacles.append((via.GetBoundingBox(), "GND"))
                added += 1
            x += PITCH
        y += PITCH

    # A grid catches most of it, but routing slices a pour into islands and a
    # grid does not know where they are. Fill, then find any island with no GND
    # connection of its own and put a via inside it.
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())

    # Islands that each have a connection but are not joined to one another are
    # still separate planes, and DRC is right to say so. Bridging every pair is
    # a graph problem; bridging each island to the MAIN plane on the opposite
    # layer solves the same thing with one rule: find a point that lies inside
    # both this island and the other layer's largest island, and put a via there.
    def islands(zone, layer):
        polys = zone.GetFilledPolysList(layer)
        out = []
        for i in range(polys.OutlineCount()):
            one = pcbnew.SHAPE_POLY_SET()
            one.AddOutline(polys.Outline(i))
            out.append((one.Area(), one))
        out.sort(key=lambda t: -t[0])
        return out

    by_layer = {}
    for zone in board.Zones():
        for layer in zone.GetLayerSet().Seq():
            by_layer.setdefault(layer, []).extend(islands(zone, layer))
    for layer in by_layer:
        by_layer[layer].sort(key=lambda t: -t[0])

    targeted = 0
    for _round in range(8):
        added_this_round = _bridge_round(board, obstacles, fits, gnd)
        targeted += added_this_round
        if added_this_round == 0:
            break
        filler.Fill(board.Zones())
    if False:
      for layer, isl in by_layer.items():
        others = [l for l in by_layer if l != layer]
        if not others or len(isl) < 2:
            continue
        main_other = by_layer[others[0]][0][1] if by_layer[others[0]] else None
        if main_other is None:
            continue
        for _area, one in isl[1:]:
            ibb = one.BBox()
            pt = None
            cands = [pcbnew.VECTOR2I((ibb.GetLeft() + ibb.GetRight()) // 2,
                                     (ibb.GetTop() + ibb.GetBottom()) // 2)]
            for fx in range(1, 20):
                for fy in range(1, 20):
                    cands.append(pcbnew.VECTOR2I(
                        ibb.GetLeft() + ibb.GetWidth() * fx // 20,
                        ibb.GetTop() + ibb.GetHeight() * fy // 20))
            for cand in cands:
                if one.Contains(cand) and main_other.Contains(cand) and fits(cand.x, cand.y):
                    pt = cand
                    break
            if pt is None:
                continue
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pt)
            via.SetDrill(pcbnew.FromMM(DRILL))
            via.SetWidth(pcbnew.FromMM(WIDTH))
            via.SetViaType(pcbnew.VIATYPE_THROUGH)
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            via.SetNetCode(gnd.GetNetCode())
            board.Add(via)
            obstacles.append((via.GetBoundingBox(), "GND"))
            targeted += 1

    if targeted:
        filler.Fill(board.Zones())

    # A GND pad whose only copper is an isolated island is electrically
    # floating, and DRC does not say so -- the pad HAS a connection, just not to
    # the ground network. On this board that hit D1's ESD array and U4's load
    # switch, either of which failing silently is worse than an open circuit.
    #
    # Drop a via right beside any such pad, landing in the opposite layer's main
    # plane, which is what ties it back in.
    filler.Fill(board.Zones())

    def main_plane(layer):
        best = None
        for zone in board.Zones():
            if zone.GetNetname() != "GND" or layer not in zone.GetLayerSet().Seq():
                continue
            polys = zone.GetFilledPolysList(layer)
            for i in range(polys.OutlineCount()):
                one = pcbnew.SHAPE_POLY_SET()
                one.AddOutline(polys.Outline(i))
                if best is None or one.Area() > best[0]:
                    best = (one.Area(), one)
        return best[1] if best else None

    planes = {L: main_plane(L) for L in (pcbnew.F_Cu, pcbnew.B_Cu)}

    # The rescue must respect the same rules as everything else: the perimeter
    # routing keepouts, and the board edge. Without this it placed vias inside
    # the antenna strip and a fifth of a millimetre from the edge.
    keepouts = []
    for zone in board.Zones():
        if zone.GetIsRuleArea() if hasattr(zone, "GetIsRuleArea") else zone.GetIsKeepout():
            kb = zone.Outline().BBox()
            keepouts.append((pcbnew.ToMM(kb.GetLeft()), pcbnew.ToMM(kb.GetTop()),
                             pcbnew.ToMM(kb.GetRight()), pcbnew.ToMM(kb.GetBottom())))

    def legal(cx, cy):
        x, y = pcbnew.ToMM(cx), pcbnew.ToMM(cy)
        need = WIDTH / 2.0 + 0.35
        if not (eL + need <= x <= eR - need and eT + need <= y <= eB - need):
            return False
        for kx0, ky0, kx1, ky1 in keepouts:
            if kx0 - need <= x <= kx1 + need and ky0 - need <= y <= ky1 + need:
                return False
        return True

    geom = _collect_geometry(board)
    drills = geom[2]

    analysis = _test_ground.analyse(board)
    floating = analysis[0] if analysis else []
    by_ref = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            by_ref[(fp.GetReference(), pad.GetNumber())] = (fp, pad)

    rescued = 0
    unfixed = []
    for ref, num, _pos, _back in floating:
        entry = by_ref.get((ref, num))
        if entry is None:
            continue
        fp, pad = entry
        pos = pad.GetPosition()
        px, py = pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
        tht = pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)
        own = ([pcbnew.F_Cu, pcbnew.B_Cu] if tht
               else [pcbnew.B_Cu if fp.IsFlipped() else pcbnew.F_Cu])
        other = [L for L in (pcbnew.F_Cu, pcbnew.B_Cu) if L not in own] or own
        both = (pcbnew.F_Cu, pcbnew.B_Cu)
        local = _near(geom, px, py, 7.0)
        local_drills = local[2]

        # Sweep outward in 0.1mm steps. The first legal spot is the shortest
        # stub, which is what we want -- a long rescue track is an antenna.
        placed = False
        r = 0.6
        while r <= 6.0 and not placed:
            for k in range(72):
                ang = 2 * math.pi * k / 72.0
                x, y = px + r * math.cos(ang), py + r * math.sin(ang)
                cx, cy = pcbnew.FromMM(x), pcbnew.FromMM(y)
                pt = pcbnew.VECTOR2I(cx, cy)
                # Cheap tests first. SHAPE_POLY_SET.Contains on a filled
                # pour walks thousands of vertices, and running it on every
                # one of the ~4000 candidates per pad made this step take
                # longer than the rest of the build together. Almost all
                # candidates die on the pure-arithmetic checks below, so ask
                # the plane last.
                if not legal(cx, cy):
                    continue
                # Two drills may not come within the hole-to-hole rule
                # however their nets relate -- a fabrication limit.
                if any(((vx - x) ** 2 + (vy - y) ** 2) ** 0.5
                       < (DRILL + vd) / 2.0 + RESCUE_HOLE
                       for vx, vy, vd in local_drills):
                    continue
                # The via barrel on both layers, then the stub from the pad
                # to it on the pad's own layer. Checking only the via is what
                # produced tracks_crossing once: the via landed fine and the
                # stub ran straight through another net.
                if not _clear_of(local, x, y, x, y, WIDTH / 2.0, both):
                    continue
                if not _clear_of(local, px, py, x, y, 0.15, tuple(own)):
                    continue
                # Only now: does this land in the opposite layer's plane?
                if not any(planes[L] is not None and planes[L].Contains(pt)
                           for L in other):
                    continue

                via = pcbnew.PCB_VIA(board)
                via.SetPosition(pt)
                via.SetDrill(pcbnew.FromMM(DRILL))
                via.SetWidth(pcbnew.FromMM(WIDTH))
                via.SetViaType(pcbnew.VIATYPE_THROUGH)
                via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                via.SetNetCode(gnd.GetNetCode())
                board.Add(via)
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pos)
                t.SetEnd(pt)
                t.SetWidth(pcbnew.FromMM(0.3))
                t.SetLayer(own[0])
                t.SetNetCode(gnd.GetNetCode())
                board.Add(t)
                # Keep the model current so two rescues cannot collide.
                geom[0].append((x, y, x, y, WIDTH / 2.0, None, "GND"))
                geom[0].append((px, py, x, y, 0.15, own[0], "GND"))
                drills.append((x, y, DRILL))
                local_drills.append((x, y, DRILL))
                obstacles.append((via.GetBoundingBox(), "GND"))
                obstacles.append((t.GetBoundingBox(), "GND"))
                print("    rescued %s pin %s with a via %.2fmm away"
                      % (ref, num, r))
                rescued += 1
                placed = True
                break
            r += 0.1
        if not placed:
            unfixed.append("%s.%s" % (ref, num))
    if rescued:
        filler.Fill(board.Zones())
        print("tied %d floating GND pads back to the plane" % rescued)
    if unfixed:
        # Silence here is how a floating ground ships. Say it.
        print("    NO ROOM for a rescue via beside: %s" % ", ".join(unfixed))

    pcbnew.SaveBoard(PCB, board)
    print("added %d grid vias at %.1fmm pitch, %d bridging islands to the main plane"
          % (added, PITCH, targeted))
    return 0


if __name__ == "__main__":
    sys.exit(main())
