#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Widen any power-rail segment that came out at signal width.

The Power net class makes the AUTOROUTER use 0.4mm, but two other things draw
tracks: join-pads.py (one fixed width for every net) and the router's own
necks. The USB-C connector's two 5V_RAW pin pairs were joined by 9mm of 0.25mm
track -- on the rail that feeds the entire board.

Rather than teach every tool about net classes, this runs once after all of
them: each power segment below the class width is widened if, and only if, the
wider track still clears every other net by the design rule. A segment that
cannot be widened is left alone and named, so a neck beside a fine-pitch pad
stays legal instead of becoming a clearance violation.

Run after join-pads.py and before stitching and the pours.
"""
import importlib.util
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(os.path.dirname(HERE), "hardware", "kicad", "ups-adaptor.kicad_pcb")

_spec = importlib.util.spec_from_file_location(
    "stitch_pcb", os.path.join(HERE, "stitch-pcb.py"))
_stitch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_stitch)

POWER = {"+5V", "5V_RAW", "+3V3", "VBUS_A"}
TARGET = 0.4


def main():
    board = pcbnew.LoadBoard(PCB)
    geom = _stitch._collect_geometry(board)
    mm = pcbnew.ToMM
    widened, stuck, stuck_len = 0, 0, 0.0
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA) or t.GetNetname() not in POWER:
            continue
        if mm(t.GetWidth()) >= TARGET - 0.005:
            continue
        ax, ay = mm(t.GetStart().x), mm(t.GetStart().y)
        bx, by = mm(t.GetEnd().x), mm(t.GetEnd().y)
        local = _stitch._near(geom, (ax + bx) / 2, (ay + by) / 2,
                              ((ax - bx) ** 2 + (ay - by) ** 2) ** 0.5 / 2 + 2.0)
        if _stitch._clear_of(local, ax, ay, bx, by, TARGET / 2.0,
                             (t.GetLayer(),), own_net=t.GetNetname()):
            t.SetWidth(pcbnew.FromMM(TARGET))
            widened += 1
        else:
            stuck += 1
            stuck_len += ((ax - bx) ** 2 + (ay - by) ** 2) ** 0.5
    pcbnew.SaveBoard(PCB, board)
    print("widened %d thin power segments to %.1fmm; %d (%.1fmm in all) have no "
          "room and stay as necks" % (widened, TARGET, stuck, stuck_len))
    return 0


if __name__ == "__main__":
    sys.exit(main())
