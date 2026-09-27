#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Give every surface-mount ground pad its escape via, before routing.

WHY THIS RUNS BEFORE THE ROUTER

Stitching happens after routing, and by then the space beside a ground pad is
gone -- the autorouter has taken it for signals, because nothing told it that
space was spoken for. On this board that left D1 pin 2 (the ESD array on the
USB-C pair) and C3 pin 2 fenced in by their own part's signal pads on one side
and routed copper on the other, with no legal via position within 6 mm on either
layer. The post-routing rescue in stitch-pcb.py could not fix them and neither
could a person without ripping up routing.

A human designer places the ground via first and routes around it. That is all
this does. Specctra export carries existing tracks across as fixed, so
Freerouting sees these as obstacles and works around them.

Through-hole ground pads are skipped: they already have copper on both layers,
so they reach whichever pour survives.

Run after place-pcb.py, before route-pcb.py.
"""
import importlib.util
import math
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.join(os.path.dirname(HERE), "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

_spec = importlib.util.spec_from_file_location(
    "stitch_pcb", os.path.join(HERE, "stitch-pcb.py"))
_stitch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_stitch)

NET = "GND"
DRILL = 0.3
WIDTH = 0.6
STUB = 0.3         # stub track width
EDGE = 0.65        # via centre to board edge
HOLE = 0.5         # hole-to-hole, edge to edge
MAX_R = 2.5        # a longer escape than this is not an escape


def main():
    board = pcbnew.LoadBoard(PCB)
    gnd = board.FindNet(NET)
    if gnd is None:
        print("no %s net" % NET)
        return 1

    geom = _stitch._collect_geometry(board)
    drills = geom[2]

    ebb = board.GetBoardEdgesBoundingBox()
    eL, eT = pcbnew.ToMM(ebb.GetLeft()), pcbnew.ToMM(ebb.GetTop())
    eR, eB = pcbnew.ToMM(ebb.GetRight()), pcbnew.ToMM(ebb.GetBottom())

    keepouts = []
    for zone in board.Zones():
        if zone.GetIsRuleArea():
            kb = zone.Outline().BBox()
            keepouts.append((pcbnew.ToMM(kb.GetLeft()), pcbnew.ToMM(kb.GetTop()),
                             pcbnew.ToMM(kb.GetRight()), pcbnew.ToMM(kb.GetBottom())))

    def legal(x, y):
        need = WIDTH / 2.0 + EDGE
        if not (eL + need <= x <= eR - need and eT + need <= y <= eB - need):
            return False
        for kx0, ky0, kx1, ky1 in keepouts:
            if kx0 - need <= x <= kx1 + need and ky0 - need <= y <= ky1 + need:
                return False
        return True

    # Most constrained first: a pad hemmed in by its own part's pads has the
    # fewest escapes, and taking them in arbitrary order lets an easy pad sit in
    # the one spot a hard pad needed.
    todo = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() != NET:
                continue
            if pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
                continue
            layer = pcbnew.B_Cu if fp.IsFlipped() else pcbnew.F_Cu
            pos = pad.GetPosition()
            px, py = pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
            crowd = sum(1 for p2 in fp.Pads() if p2.GetNetname() != NET)
            todo.append((-crowd, fp.GetReference(), pad.GetNumber(),
                         pos, px, py, layer))
    todo.sort(key=lambda t: (t[0], t[1], t[2]))

    placed, failed = 0, []
    for _c, ref, num, pos, px, py, layer in todo:
        local = _stitch._near(geom, px, py, MAX_R + 2.0)
        local_drills = local[2]
        done = False
        r = 0.5
        while r <= MAX_R and not done:
            for k in range(72):
                ang = 2 * math.pi * k / 72.0
                x, y = px + r * math.cos(ang), py + r * math.sin(ang)
                if not legal(x, y):
                    continue
                if any(((vx - x) ** 2 + (vy - y) ** 2) ** 0.5
                       < (DRILL + vd) / 2.0 + HOLE for vx, vy, vd in local_drills):
                    continue
                if not _stitch._clear_of(local, x, y, x, y, WIDTH / 2.0,
                                         (pcbnew.F_Cu, pcbnew.B_Cu), own_net=NET):
                    continue
                if not _stitch._clear_of(local, px, py, x, y, STUB / 2.0,
                                         (layer,), own_net=NET):
                    continue

                pt = pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))
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
                t.SetWidth(pcbnew.FromMM(STUB))
                t.SetLayer(layer)
                t.SetNetCode(gnd.GetNetCode())
                board.Add(t)

                geom[0].append((x, y, x, y, WIDTH / 2.0, None, NET))
                geom[0].append((px, py, x, y, STUB / 2.0, layer, NET))
                drills.append((x, y, DRILL))
                local_drills.append((x, y, DRILL))
                placed += 1
                done = True
                break
            r += 0.1
        if not done:
            failed.append("%s.%s" % (ref, num))

    pcbnew.SaveBoard(PCB, board)
    print("reserved %d ground escape vias before routing" % placed)
    if failed:
        print("  no room beside: %s" % ", ".join(failed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
