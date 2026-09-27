#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find floating grounds: GND copper that is not part of the ground network.

WHY THIS IS NOT JUST DRC

DRC's unconnected check asks "does this item have a connection?". A pad sitting
in an isolated pour island HAS one, so DRC stays quiet while the pad is
electrically floating. The question that matters is different: "is this item in
the same electrical network as the main ground plane?"

This builds that network explicitly -- every GND pad, via, track and pour island
as nodes, joined where they physically touch -- then finds the component holding
the largest island and reports everything outside it.

A floating ground is not a cosmetic defect. A decoupling capacitor whose ground
end reaches nothing decouples nothing; an ESD array with a floating ground shunts
nothing anywhere.

    $KICAD_PY tools/test/test_ground.py
"""
import os
import sys

import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")
NET = "GND"
TOL = pcbnew.FromMM(0.05)      # two points this close are the same point


class UF:
    def __init__(self):
        self.p = {}

    def find(self, a):
        self.p.setdefault(a, a)
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[ra] = rb


def mm(v):
    return pcbnew.ToMM(v)


def analyse(board):
    """Returns (floating_pads, orphan_islands, main_island).

    floating_pads: [(ref, padnum, VECTOR2I, is_back)]
    Shared with tools/route-guide.py so the drawing and the check cannot
    disagree about what is floating."""
    net = board.FindNet(NET)
    if net is None:
        return None

    # --- nodes: pour islands -------------------------------------------
    islands = []          # (key, layer, polygon, area_mm2)
    for z in board.Zones():
        if z.GetNetname() != NET:
            continue
        for layer in z.GetLayerSet().Seq():
            polys = z.GetFilledPolysList(layer)
            for i in range(polys.OutlineCount()):
                one = pcbnew.SHAPE_POLY_SET()
                one.AddOutline(polys.Outline(i))
                islands.append(("island%d" % len(islands), layer, one,
                                mm(mm(one.Area()))))
    if not islands:
        return None

    uf = UF()
    for key, _l, _p, _a in islands:
        uf.find(key)

    def island_at(pt, layer):
        for key, ilayer, poly, _a in islands:
            if ilayer == layer and poly.Contains(pt):
                return key
        return None

    # --- pads ------------------------------------------------------------
    pads = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() != NET:
                continue
            tht = pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)
            layers = ([pcbnew.F_Cu, pcbnew.B_Cu] if tht
                      else [pcbnew.B_Cu if fp.IsFlipped() else pcbnew.F_Cu])
            key = "pad:%s.%s" % (fp.GetReference(), pad.GetNumber())
            pads.append((key, pad.GetPosition(), layers, fp.GetReference(),
                         pad.GetNumber(), fp.IsFlipped()))
            uf.find(key)
            for L in layers:
                isl = island_at(pad.GetPosition(), L)
                if isl:
                    uf.union(key, isl)

    # --- tracks and vias --------------------------------------------------
    endpoints = []        # (key, position, layers) for joining coincident ends
    for t in board.GetTracks():
        if t.GetNetname() != NET:
            continue
        key = "trk%d" % id(t)
        uf.find(key)
        if isinstance(t, pcbnew.PCB_VIA):
            for L in (pcbnew.F_Cu, pcbnew.B_Cu):
                isl = island_at(t.GetPosition(), L)
                if isl:
                    uf.union(key, isl)
            endpoints.append((key, t.GetPosition(), (pcbnew.F_Cu, pcbnew.B_Cu)))
        else:
            L = t.GetLayer()
            for p in (t.GetStart(), t.GetEnd()):
                isl = island_at(p, L)
                if isl:
                    uf.union(key, isl)
                endpoints.append((key, p, (L,)))

    # join anything meeting at the same point on a shared layer
    for i, (k1, p1, l1) in enumerate(endpoints):
        for k2, p2, l2 in endpoints[i + 1:]:
            if k1 == k2 or not set(l1) & set(l2):
                continue
            if abs(p1.x - p2.x) <= TOL and abs(p1.y - p2.y) <= TOL:
                uf.union(k1, k2)
    for key, pos, layers, _r, _n, _b in pads:
        for k2, p2, l2 in endpoints:
            if not set(layers) & set(l2):
                continue
            if abs(pos.x - p2.x) <= TOL and abs(pos.y - p2.y) <= TOL:
                uf.union(key, k2)

    # --- the plane is the component holding the largest island ------------
    main_island = max(islands, key=lambda t: t[3])
    plane = uf.find(main_island[0])

    floating_pads = [(r, n, p, b) for key, p, _l, r, n, b in pads
                     if uf.find(key) != plane]
    orphan_islands = [(k, l, a) for k, l, _p, a in islands if uf.find(k) != plane]
    return floating_pads, orphan_islands, main_island


def main():
    board = pcbnew.LoadBoard(PCB)
    res = analyse(board)
    if res is None:
        print("  FAIL no %s net or pour" % NET)
        return 1
    floating_pads, orphan_islands, main_island = res
    pads_n = sum(1 for fp in board.GetFootprints() for p in fp.Pads()
                 if p.GetNetname() == NET)
    islands_n = len(orphan_islands)

    print("  ground network: %d GND pads, main plane %.0f mm2 on %s"
          % (pads_n, main_island[3], board.GetLayerName(main_island[1])))

    fails = 0
    if floating_pads:
        print("  FAIL %d GND pads are NOT connected to the ground plane:"
              % len(floating_pads))
        for r, n, _p, _b in sorted(floating_pads, key=lambda t: (t[0], t[1])):
            print("        %s pin %s" % (r, n))
        fails += 1
    else:
        print("  ok   every GND pad reaches the ground plane")

    big = [(k, l, a) for k, l, a in orphan_islands if a >= 2.0]
    if big:
        total = sum(a for _k, _l, a in big)
        print("  WARN %d pour islands (%.0f mm2) are not tied to the plane:"
              % (len(big), total))
        for k, l, a in sorted(big, key=lambda t: -t[2])[:6]:
            print("        %6.1f mm2 on %s" % (a, board.GetLayerName(l)))
        print("       Dead copper -- tie them in or let island removal drop them.")
    else:
        print("  ok   no significant orphan pour islands")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
