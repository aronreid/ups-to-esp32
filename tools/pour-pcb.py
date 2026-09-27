#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add ground pours to both layers and fill them.

The bottom pour is not cosmetic. A 12 Mbps USB pair wants continuous ground
directly beneath it; that is the reason the passives were flipped to the bottom
in the first place, leaving it mostly empty. The top pour is ordinary
stitching -- useful, but the bottom one is the one that matters.

Run after routing, and re-run after any change that moves copper.
"""
import os
import sys

import pcbnew

HW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                  "hardware", "kicad")
PCB = os.path.join(HW, "ups-adaptor.kicad_pcb")

INSET = 0.3      # pull the pour in from the board edge


def main():
    board = pcbnew.LoadBoard(PCB)

    zones = list(board.Zones())
    if not zones:
        print("no zones on the board -- gen-pcb.py defines them")
        return 1

    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)

    for z in zones:
        print("  filled pour on %s (net %s)" % (board.GetLayerName(z.GetLayerSet().Seq()[0]), z.GetNetname()))
    print("board has %d track/via objects" % len(board.GetTracks()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
