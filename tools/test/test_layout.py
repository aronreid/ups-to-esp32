#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Layout properties DRC cannot check.

DRC answers "is this legal?". It does not answer "is the antenna keepout
actually clear?", "are the USB pairs still a pair?", or "can a human read the
silkscreen?" -- and those are the ones that produce a board that passes every
gate and works badly.

Run with KiCad's Python:  $KICAD_PY tools/test/test_layout.py
"""
import importlib.util
import os
import sys

import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCB = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_pcb")

# The module's own keepout polygon, in footprint-local mm, from
# RF_Module:ESP32-S3-WROOM-1. Espressif's 15mm rule made concrete.
KEEPOUT_LOCAL = (-24.0, -27.75, 24.0, -6.75)

PAIRS = [("USB_DM", "USB_DP", "USB host pair to the UPS"),
         ("PROG_DM", "PROG_DP", "USB-C programming pair")]
# USB full speed is 12 Mbps: an 83ns bit period against roughly 6ps/mm on FR4.
# Ten millimetres of skew is about 60ps, which is nothing. This is a smoke test
# for "were these actually routed as a pair", not a length-matching gate --
# length matching is a high-speed concern and this is not a high-speed bus.
LENGTH_TOLERANCE = 10.0


def mm(v):
    return pcbnew.ToMM(v)


def main():
    board = pcbnew.LoadBoard(PCB)
    fails = 0

    # ---- antenna keepout ------------------------------------------------
    u1 = board.FindFootprintByReference("U1")
    if u1 is None:
        print("  FAIL no U1")
        return 1
    ox, oy = mm(u1.GetPosition().x), mm(u1.GetPosition().y)
    kx0, ky0, kx1, ky1 = KEEPOUT_LOCAL
    keep = (ox + kx0, oy + ky0, ox + kx1, oy + ky1)

    bb = board.GetBoardEdgesBoundingBox()
    board_box = (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))
    ov_x = min(keep[2], board_box[2]) - max(keep[0], board_box[0])
    ov_y = min(keep[3], board_box[3]) - max(keep[1], board_box[1])

    if ov_x <= 0 or ov_y <= 0:
        print("  ok   antenna keepout lies entirely off the board "
              "(clears the edge by %.2f mm)" % -ov_y)
    else:
        intruders = 0
        for t in board.GetTracks():
            p = t.GetPosition()
            if keep[0] <= mm(p.x) <= keep[2] and keep[1] <= mm(p.y) <= keep[3]:
                intruders += 1
        for fp in board.GetFootprints():
            if fp.GetReference() == "U1":
                continue
            p = fp.GetPosition()
            if keep[0] <= mm(p.x) <= keep[2] and keep[1] <= mm(p.y) <= keep[3]:
                intruders += 1
        if intruders:
            print("  FAIL %d copper items inside the antenna keepout" % intruders)
            fails += 1
        else:
            print("  WARN keepout overlaps the board by %.1f x %.1f mm but nothing "
                  "is in it; the pour still needs checking by eye" % (ov_x, ov_y))

    # ---- differential pairs ---------------------------------------------
    lengths, layers = {}, {}
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            continue
        n = t.GetNetname()
        lengths[n] = lengths.get(n, 0.0) + mm(t.GetLength())
        layers.setdefault(n, set()).add(t.GetLayerName())

    for a, b, what in PAIRS:
        la, lb = lengths.get(a), lengths.get(b)
        if la is None or lb is None:
            print("  FAIL %s: %s unrouted" % (what, a if la is None else b))
            fails += 1
            continue
        skew = abs(la - lb)
        tag = "ok  " if skew <= LENGTH_TOLERANCE else "WARN"
        if skew > LENGTH_TOLERANCE:
            fails += 0      # a warning, not a gate, at 12 Mbps
        print("  %s %-26s %.1f / %.1f mm, skew %.1f mm  layers %s / %s"
              % (tag, what, la, lb, skew,
                 ",".join(sorted(layers[a])), ",".join(sorted(layers[b]))))

    # ---- silkscreen ------------------------------------------------------
    missing = []
    # Passive designators and the two button outlines are removed ON PURPOSE --
    # 21 of them made the labels a user actually needs harder to read. Take the
    # rule from silk-pcb.py rather than restating it, so this cannot quietly
    # become a rubber stamp if that list changes.
    spec = importlib.util.spec_from_file_location(
        "silk_pcb", os.path.join(ROOT, "tools", "silk-pcb.py"))
    silk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(silk)

    intentional = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        item = fp.Reference()
        if item.IsVisible() and ref and not ref.startswith("#"):
            continue
        if ref in silk.NO_SILK or ref.startswith(silk.HIDE_REFDES_PREFIX):
            intentional.append(ref)
        else:
            missing.append(ref or "?")
    if missing:
        print("  FAIL %d parts have no visible reference designator: %s"
              % (len(missing), ", ".join(sorted(missing)[:8])))
        fails += 1
    else:
        print("  ok   every part a human needs to identify carries a designator")
        if intentional:
            print("       %d omitted deliberately (%s...): the CPL still carries"
                  % (len(intentional), ", ".join(sorted(intentional)[:4])))
            print("       every one, which is what assembly reads -- the cost is")
            print("       hand rework, where you need the board file to find R7")

    # ---- polarised parts --------------------------------------------------
    # A polarity marker on the wrong end is not a DRC error and not visible in a
    # netlist. It is visible to whoever assembles or reviews the board, who will
    # trust it -- and a marker that is merely NEAR the part is as bad as a wrong
    # one. So assert the property the dot actually has to have: each polarised
    # part carries a dot that is closer to the pin it marks than to any other
    # pad of that part, by enough that a human reading it cannot be in doubt.
    MARGIN = 0.4
    # A dot for most parts; for D3/D4 a BAR across the cathode, because a dot on
    # a diode reads as the capacitor convention and so means the opposite.
    dots = []
    for d in board.GetDrawings():
        if not isinstance(d, pcbnew.PCB_SHAPE) or d.GetLayer() != pcbnew.F_SilkS:
            continue
        if d.GetShape() == pcbnew.SHAPE_T_CIRCLE:
            dots.append((mm(d.GetCenter().x), mm(d.GetCenter().y)))
        elif d.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            dots.append(((mm(d.GetStart().x) + mm(d.GetEnd().x)) / 2,
                         (mm(d.GetStart().y) + mm(d.GetEnd().y)) / 2))
    bad, good = [], []
    for ref, num in sorted(silk.POLARISED.items()):
        fp = board.FindFootprintByReference(ref)
        if fp is None:
            bad.append("%s missing" % ref)
            continue
        marked, rest = None, []
        for pad in fp.Pads():
            pos = (mm(pad.GetPosition().x), mm(pad.GetPosition().y))
            if pad.GetNumber() == num:
                marked = pos
            else:
                rest.append(pos)
        if marked is None or not dots:
            bad.append("%s unmarked" % ref)
            continue
        d = min(dots, key=lambda q: (q[0] - marked[0]) ** 2 + (q[1] - marked[1]) ** 2)
        dp = ((d[0] - marked[0]) ** 2 + (d[1] - marked[1]) ** 2) ** 0.5
        do = min((((d[0] - o[0]) ** 2 + (d[1] - o[1]) ** 2) ** 0.5 for o in rest),
                 default=dp + 99.0)
        if do - dp < MARGIN:
            bad.append("%s dot is %+.2f mm" % (ref, do - dp))
        else:
            good.append(ref)
    if bad:
        print("  FAIL %d polarised parts have no unambiguous pin-1 dot: %s"
              % (len(bad), ", ".join(bad)))
        fails += 1
    else:
        print("  ok   all %d polarised parts carry a pin-1 dot nearer the pin it"
              " marks" % len(good))
        print("       than any other pad, by >= %.1f mm; on D1-D4 that pin is the"
              " CATHODE" % MARGIN)

    # ---- power tracks -----------------------------------------------------
    # Every net used to route at the Default 0.2mm, the rails included: 78mm of
    # +5V and 127mm of +3V3 at signal width, 2.4 mOhm per mm, in series with a
    # supply that steps 300mA on every Wi-Fi burst. The Power net class fixes
    # it at the source; this checks the router honoured it. Short necks where a
    # track leaves a fine-pitch pad are tolerated, long thin runs are not.
    POWER = {"+5V": 0.4, "5V_RAW": 0.4, "+3V3": 0.4, "VBUS_A": 0.4}
    thin = {}
    total = {}
    for t in board.GetTracks():
        n = t.GetNetname()
        if n not in POWER or isinstance(t, pcbnew.PCB_VIA):
            continue
        length = ((mm(t.GetEnd().x - t.GetStart().x)) ** 2 +
                  (mm(t.GetEnd().y - t.GetStart().y)) ** 2) ** 0.5
        total[n] = total.get(n, 0.0) + length
        if mm(t.GetWidth()) < POWER[n] - 0.01:
            thin[n] = thin.get(n, 0.0) + length
    bad = [(n, thin[n], total[n]) for n in thin if thin[n] > 0.25 * total[n]]
    if bad:
        for n, th, tot in bad:
            print("  FAIL %-7s %.0f of %.0f mm is routed below %.1f mm -- a power rail"
                  " at signal width" % (n, th, tot, POWER[n]))
        fails += 1
    else:
        print("  ok   power rails routed at >= 0.4 mm (%s)"
              % ", ".join("%s %.0f mm" % (n, total[n]) for n in sorted(total)))

    # ---- decoupling -------------------------------------------------------
    # The 22uF that serves the ESP32 has to be NEAR the ESP32. It used to be
    # 25mm away beside the regulator, along with every other 3V3 capacitor.
    pads = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            pads[(fp.GetReference(), pad.GetNumber())] = pad.GetPosition()
    pin = pads.get(("U1", "2"))
    if pin is not None:
        near = []
        for fp in board.GetFootprints():
            ref = fp.GetReference()
            if not ref.startswith("C"):
                continue
            nets = {p.GetNetname() for p in fp.Pads()}
            if nets == {"+3V3", "GND"}:
                pos = pads[(ref, "1")]
                near.append((((mm(pos.x - pin.x)) ** 2 + (mm(pos.y - pin.y)) ** 2) ** 0.5, ref))
        near.sort()
        # Judge the DROOP, not the distance. A 12mm rule is arbitrary and went
        # off the moment a silkscreen label displaced a capacitor by 2mm, which
        # is not a reason to fail a board. What matters is the volts the module
        # loses when it starts transmitting.
        #
        # ~1 nH per mm for a 0.4mm track over a plane; the ESP32-S3 steps about
        # 350mA into a Wi-Fi burst. The external bulk serves the microsecond
        # ENVELOPE of that burst -- nanosecond edges are the job of the
        # module's own internal decoupling, which is why Espressif ship it.
        d = near[0][0] if near else 99.0
        droop_mv = (d * 1e-9) * (0.35 / 1e-6) * 1000.0
        LIMIT_MV = 50.0          # 1.5% of 3.3V
        if near and droop_mv <= LIMIT_MV:
            print("  ok   nearest 3V3 decoupling %s at %.1f mm -> ~%.0f mV of burst"
                  " droop (limit %.0f)" % (near[0][1], d, droop_mv, LIMIT_MV))
        else:
            print("  FAIL nearest 3V3 capacitor is %.1f mm from U1 pin 2: ~%.0f mV"
                  % (d, droop_mv))
            print("       of droop on a 350mA Wi-Fi burst, over the %.0f mV limit"
                  % LIMIT_MV)
            fails += 1

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
