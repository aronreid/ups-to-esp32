#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join duplicate pads within a footprint.

A tact switch has two pins per side, bonded inside the part. KiCad models them
as two pads with the same number and net, and DRC wants copper to both -- but an
autorouter that has already reached one sees no reason to reach the other, and
on this board the spare pads sit against the top keepout with nowhere to go.

The fix is what a person would draw: a straight track between them. They are the
same net, so this cannot short anything; the only question is whether the path is
clear, which is checked.

Run after routing, before stitching.
"""
import importlib.util
import math
import os
import sys

import pcbnew

# The geometry helpers live in stitch-pcb.py, which is a script, not a module.
_spec = importlib.util.spec_from_file_location(
    "stitch_pcb", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "stitch-pcb.py"))
_stitch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_stitch)

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

WIDTH = 0.25       # mm
CLEAR = 0.25       # mm from anything on another net
MAX_LEN = 9.0      # mm; duplicate pads of one part are close together

# Nets carried by a filled pour need no joining track: every pad on them already
# reaches the plane. Joining them anyway produced a 17.5mm GND track straight
# across the antenna keepout, which is worse than the problem it solved.
POURED = {"GND"}


def pad_layers(pad):
    """The copper layers this pad actually has.

    This used to be asked of the whole FOOTPRINT -- "does it contain any
    through-hole pad?" -- and the answer was used for every pad in it. J1, the
    USB-C receptacle, has through-hole shield legs and surface-mount signal
    pads, so a join between two F.Cu-only 5V_RAW pads was allowed to be drawn on
    B.Cu, where it touched neither of them. DRC called it unconnected and it
    was: a 4.9mm track joining nothing."""
    return tuple(L for L in pad.GetLayerSet().Seq()
                 if L in (pcbnew.F_Cu, pcbnew.B_Cu))


def path_clear(pts, net, geom, layer):
    """True if every segment of the path clears other nets on `layer`.

    This used to compare bounding boxes and ignore the layer, which is wrong
    twice over. A long diagonal track's box is the whole rectangle it spans, so
    it vetoed ground it never touched; and copper on the front blocked a path
    drawn on the back. Between them they were the reason SW2's second IO0 pin
    never got joined and the board shipped one unconnected net."""
    for a, b in zip(pts, pts[1:]):
        ax, ay = pcbnew.ToMM(a.x), pcbnew.ToMM(a.y)
        bx, by = pcbnew.ToMM(b.x), pcbnew.ToMM(b.y)
        if not _stitch._clear_of(geom, ax, ay, bx, by, WIDTH / 2.0, (layer,),
                                 own_net=net, clr=CLEAR):
            return False
    return True


def candidate_paths(a, b):
    """Straight first, then a three-segment detour at increasing offsets.

    Two bonded switch pins can have other nets routed straight between them --
    on this board UART_TX, UART_RX and +3V3 all cross between SW2's pins -- so a
    straight join is not always possible. A detour around the outside is what a
    person would draw, and it is the same electrical result."""
    V = pcbnew.VECTOR2I
    yield [a, b]
    vertical = abs(b.y - a.y) > abs(b.x - a.x)
    for mm_off in (1.5, 2.0, 2.5, 3.0, 3.5):
        off = pcbnew.FromMM(mm_off)
        for sign in (1, -1):
            d = off * sign
            if vertical:
                yield [a, V(a.x + d, a.y), V(b.x + d, b.y), b]
            else:
                yield [a, V(a.x, a.y + d), V(b.x, b.y + d), b]


VIA_DRILL = 0.3
VIA_WIDTH = 0.6
VIA_MAX_R = 2.5
HOLE_TO_HOLE = 0.50    # JLC wants 0.45mm between plated holes


def _via_spot(geom, net, pos, layers_needed):
    """A via position near `pos` whose stub from the pad is clear on every layer
    in `layers_needed`. Returns (x, y) in mm, or None."""
    px, py = pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
    local = _stitch._near(geom, px, py, VIA_MAX_R + 2.0)
    drills = local[2]
    r = 0.5
    while r <= VIA_MAX_R:
        for k in range(72):
            ang = 2 * math.pi * k / 72.0
            x, y = px + r * math.cos(ang), py + r * math.sin(ang)
            # Hole to hole is a drill-breakage limit and applies between any two
            # plated holes whatever their nets. Copper clearance alone let a via
            # land 0.32mm from a header hole.
            if any(((hx - x) ** 2 + (hy - y) ** 2) ** 0.5
                   < (VIA_DRILL + hd) / 2.0 + HOLE_TO_HOLE
                   for hx, hy, hd in drills):
                continue
            if not _stitch._clear_of(local, x, y, x, y, VIA_WIDTH / 2.0,
                                     (pcbnew.F_Cu, pcbnew.B_Cu), own_net=net):
                continue
            if all(_stitch._clear_of(local, px, py, x, y, WIDTH / 2.0, (L,),
                                     own_net=net) for L in layers_needed):
                return (x, y)
        r += 0.1
    return None


def _add_track(board, geom, net, a, b, layer):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(a)
    t.SetEnd(b)
    t.SetWidth(pcbnew.FromMM(WIDTH))
    t.SetLayer(layer)
    t.SetNetCode(board.FindNet(net).GetNetCode())
    board.Add(t)
    geom[0].append((pcbnew.ToMM(a.x), pcbnew.ToMM(a.y),
                    pcbnew.ToMM(b.x), pcbnew.ToMM(b.y), WIDTH / 2.0, layer, net))


def _add_via(board, geom, net, pt):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pt)
    v.SetDrill(pcbnew.FromMM(VIA_DRILL))
    v.SetWidth(pcbnew.FromMM(VIA_WIDTH))
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNetCode(board.FindNet(net).GetNetCode())
    board.Add(v)
    x, y = pcbnew.ToMM(pt.x), pcbnew.ToMM(pt.y)
    geom[0].append((x, y, x, y, VIA_WIDTH / 2.0, None, net))
    geom[2].append((x, y, VIA_DRILL))


def via_join(board, geom, net, a, a_layers, b, b_layers):
    """Join two pads that share no routable layer, by dropping to the other side.

    A via beside each pad, a short stub from pad to via on the pad's own layer,
    and the long run between the vias on the transfer layer. This is what J1's
    two 5V_RAW positions need: the receptacle's own signal pads block F.Cu, and
    the pads are F.Cu only, so no single-layer join exists."""
    for transfer in (pcbnew.B_Cu, pcbnew.F_Cu):
        va = _via_spot(geom, net, a, set(a_layers) | {transfer})
        if va is None:
            continue
        vb = _via_spot(geom, net, b, set(b_layers) | {transfer})
        if vb is None:
            continue
        pa = pcbnew.VECTOR2I(pcbnew.FromMM(va[0]), pcbnew.FromMM(va[1]))
        pb = pcbnew.VECTOR2I(pcbnew.FromMM(vb[0]), pcbnew.FromMM(vb[1]))
        if not path_clear([pa, pb], net, geom, transfer):
            continue
        _add_via(board, geom, net, pa)
        _add_via(board, geom, net, pb)
        _add_track(board, geom, net, a, pa, a_layers[0])
        _add_track(board, geom, net, b, pb, b_layers[0])
        _add_track(board, geom, net, pa, pb, transfer)
        return True
    return False


def main():
    board = pcbnew.LoadBoard(PCB)

    # Obstacles on other nets, as real geometry, captured before anything is
    # added.
    geom = _stitch._collect_geometry(board)

    groups = []
    for fp in board.GetFootprints():
        by_net = {}
        for pad in fp.Pads():
            n = pad.GetNetname()
            if not n:
                continue
            by_net.setdefault(n, []).append((pad.GetPosition(), pad_layers(pad)))
        for net, entries in by_net.items():
            if net in POURED:
                continue
            if len(entries) > 1:
                # Only a layer that EVERY pad in the group actually has can
                # carry the join. Two through-hole pads share both, so the join
                # may take either side -- and often the far side is the clear
                # one: SW2's IO0 pins have UART_TX, UART_RX and +3V3 crossing
                # between them on the front and nothing in the way on the back.
                shared = set(entries[0][1])
                for _pos, ls in entries[1:]:
                    shared &= set(ls)
                if not shared:
                    continue
                layers = [L for L in (pcbnew.F_Cu, pcbnew.B_Cu) if L in shared]
                groups.append((fp.GetReference(), net, layers, entries))

    added = 0
    detoured = 0
    viaed = 0
    skipped = []
    for ref, net, layers, entries in groups:
        anchor, anchor_layers = entries[0]
        # Stacked pads: J1's 5V_RAW is four pads at two positions (A4/B9 and
        # A9/B4 coincide), so joining the anchor to each of the other three
        # asked for the SAME join twice and left two pairs of vias sitting on
        # top of each other -- two holes drilled at one point, which is a
        # fabrication defect DRC does not report.
        joined_to = set()
        for p, p_layers in entries[1:]:
            if anchor == p:
                continue
            here = (p.x, p.y)
            if here in joined_to:
                continue
            joined_to.add(here)
            span = ((pcbnew.ToMM(p.x - anchor.x)) ** 2 +
                    (pcbnew.ToMM(p.y - anchor.y)) ** 2) ** 0.5
            if span > MAX_LEN:
                continue
            chosen = None
            layer = layers[0]
            for try_layer in layers:
                for path in candidate_paths(anchor, p):
                    if path_clear(path, net, geom, try_layer):
                        chosen, layer = path, try_layer
                        break
                if chosen:
                    break
            if chosen is None:
                # No single layer carries it. Drop to the other side: a via
                # beside each pad, the run between them on the transfer layer.
                # J1's two 5V_RAW positions are like this -- the connector's own
                # signal pads block F.Cu and the pads themselves are F.Cu only,
                # so nothing on one layer can join them.
                if via_join(board, geom, net, anchor, anchor_layers,
                            p, p_layers):
                    added += 1
                    viaed += 1
                    continue
                skipped.append("%s/%s" % (ref, net))
                continue
            for sa, sb in zip(chosen, chosen[1:]):
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(sa)
                t.SetEnd(sb)
                t.SetWidth(pcbnew.FromMM(WIDTH))
                t.SetLayer(layer)
                t.SetNetCode(board.FindNet(net).GetNetCode())
                board.Add(t)
                geom[0].append((pcbnew.ToMM(sa.x), pcbnew.ToMM(sa.y),
                                pcbnew.ToMM(sb.x), pcbnew.ToMM(sb.y),
                                WIDTH / 2.0, layer, net))
            added += 1
            if len(chosen) > 2:
                detoured += 1

    pcbnew.SaveBoard(PCB, board)
    print("joined %d duplicate pad pairs (%d needed a detour, %d needed vias), "
          "%d candidate footprint nets" % (added, detoured, viaed, len(groups)))
    if skipped:
        print("  no clear path for: %s" % ", ".join(skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
