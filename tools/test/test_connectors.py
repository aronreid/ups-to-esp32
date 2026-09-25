#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin down the USB connector pinouts.

A wrong pin on a USB receptacle is a dead board, and it is the kind of error
that no other check here would catch: DRC is happy, the netlist is
self-consistent, the ground plane is perfect, and the board does nothing when
you plug the UPS in. The footprints come from a KiCad library and the net
assignments from a generator, so nothing else asserts that the two agree with
the USB specification.

J2 is an XKB U231-091N-4BLRA00-S, which is a USB **3.0** 9-contact Type-A used
here as a USB 2.0 port -- it is the smallest surface-mount USB-A in the library.
Contacts 1-4 are the standard USB 2.0 positions; 5-9 are SuperSpeed and are
deliberately left unconnected. This test states that intent so nobody "fixes"
the unconnected pads later.

    $KICAD_PY tools/test/test_connectors.py
"""
import os
import sys

import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")

# USB 2.0 Type-A receptacle, per the specification.
USB_A = {"1": "VBUS_A", "2": "USB_DM", "3": "USB_DP", "4": "GND"}
USB_A_UNUSED = {"5", "6", "7", "8", "9"}     # SuperSpeed, not used at 12 Mbps

# USB-C receptacle. Both rows carry the same signals so the plug works either
# way up; CC1 and CC2 must stay SEPARATE, each with its own 5.1k to ground, or
# the port advertises the wrong thing to a source.
USB_C = {
    "A1": "GND", "A4": "5V_RAW", "A5": "CC1", "A6": "PROG_DP",
    "A7": "PROG_DM", "A9": "5V_RAW", "A12": "GND",
    "B1": "GND", "B4": "5V_RAW", "B5": "CC2", "B6": "PROG_DP",
    "B7": "PROG_DM", "B9": "5V_RAW", "B12": "GND",
}


def check(board, ref, expect, unused=()):
    fp = next((f for f in board.GetFootprints() if f.GetReference() == ref), None)
    if fp is None:
        print("  FAIL %s is not on the board" % ref)
        return 1
    nets = {}
    for pad in fp.Pads():
        nets.setdefault(pad.GetNumber(), set()).add(pad.GetNetname())

    bad = 0
    for num, want in sorted(expect.items()):
        got = nets.get(num)
        if got is None:
            print("  FAIL %s pad %s is missing from the footprint" % (ref, num))
            bad += 1
        elif got != {want}:
            print("  FAIL %s pad %s carries %s, expected %s"
                  % (ref, num, "/".join(sorted(got)) or "nothing", want))
            bad += 1
    for num in sorted(unused):
        got = nets.get(num, set())
        if got and got != {""}:
            print("  FAIL %s pad %s should be unconnected, carries %s"
                  % (ref, num, "/".join(sorted(got))))
            bad += 1

    if not bad:
        print("  ok   %s: %d pins match the USB specification%s"
              % (ref, len(expect),
                 ", %d SuperSpeed pads left unconnected as intended" % len(unused)
                 if unused else ""))
    return bad


def main():
    board = pcbnew.LoadBoard(PCB)
    bad = 0
    bad += check(board, "J2", USB_A, USB_A_UNUSED)
    bad += check(board, "J1", USB_C)

    # CC1 and CC2 tied together is a classic and silent error: the board then
    # looks like a different kind of device to whatever it is plugged into.
    cc = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() in ("CC1", "CC2"):
                cc.setdefault(pad.GetNetname(), []).append(
                    "%s.%s" % (fp.GetReference(), pad.GetNumber()))
    if set(cc) != {"CC1", "CC2"}:
        print("  FAIL expected both CC1 and CC2, found %s" % sorted(cc))
        bad += 1
    else:
        print("  ok   CC1 and CC2 are separate nets (%s / %s)"
              % (", ".join(sorted(set(cc["CC1"]))), ", ".join(sorted(set(cc["CC2"])))))

    # A receptacle whose mouth sits INSIDE the board edge cannot be fully mated:
    # the plug's overmold is taller than the receptacle and hangs below the PCB
    # surface, so it strikes the board edge first. Rev A had both mouths 1.3mm
    # inside -- on USB-C that is most of the contact wipe, on the port that
    # powers and programs the board. The body is the footprint's F.Fab outline.
    ebb = board.GetBoardEdgesBoundingBox()
    edge = pcbnew.ToMM(ebb.GetBottom())
    for ref in ("J1", "J2"):
        fp = next(f for f in board.GetFootprints() if f.GetReference() == ref)
        front = None
        for item in fp.GraphicalItems():
            if item.GetLayer() == pcbnew.F_Fab:
                y = pcbnew.ToMM(item.GetBoundingBox().GetBottom())
                front = y if front is None else max(front, y)
        if front is None:
            print("  WARN %s has no F.Fab outline; cannot check its mouth" % ref)
        elif front < edge - 0.05:
            print("  FAIL %s mouth is %.2f mm INSIDE the board edge: a plug's overmold"
                  % (ref, edge - front))
            print("       hits the PCB before the connector seats")
            bad += 1
        else:
            print("  ok   %s mouth reaches the board edge (%.2f mm past it)"
                  % (ref, front - edge))

    print()
    if bad:
        print("  %d connector error(s) -- this is the class of defect that"
              " makes a board dead on arrival" % bad)
        return 1
    print("  connector pinouts match the USB specification")
    return 0


if __name__ == "__main__":
    sys.exit(main())
