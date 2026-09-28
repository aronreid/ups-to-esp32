#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Power budget arithmetic, with margins called out.

Not a simulation -- a linear regulator and a polyfuse are arithmetic, and the
useful question is whether the chosen parts have headroom, not what the
waveforms look like.
"""
import importlib.util
import os
import sys

VIN, V33 = 5.0, 3.3

# 3.3V rail consumers, worst case
LOADS_33 = [("ESP32-S3 Wi-Fi TX peak", 0.350),
            ("CH340C", 0.020),
            ("OLED (when fitted)", 0.020),
            ("status LEDs", 0.010)]
UPS_VBUS = 0.100          # UPS USB interface, switched branch
POLYFUSE_HOLD = 2.000     # F1
# AP2171 datasheet: "1.5A accurate current limiting", 1.0A is only the
# RECOMMENDED continuous load. This constant used to be 1.0 -- read off a
# distributor listing -- which made the fuse margin look like 43% when it is
# nearer 5%. See the fault-case note below for why that is still acceptable.
SWITCH_LIMIT = 1.500
LDO_PKG_RTH = 60.0        # SOT-223 with >=100mm2 pour, degC/W

def main():
    i33 = sum(v for _, v in LOADS_33)
    print("  3V3 rail:")
    for n, v in LOADS_33:
        print("    %-26s %6.0f mA" % (n, v * 1000))
    print("    %-26s %6.0f mA" % ("total", i33 * 1000))

    # A linear regulator draws roughly its output current on the input side.
    i5 = i33 + UPS_VBUS
    pd = (VIN - V33) * i33
    rise = pd * LDO_PKG_RTH
    print("\n  AP2114H dissipation  %.2f W  ->  ~%.0f C rise over ambient" % (pd, rise))
    print("  5V rail total        %.0f mA  (3V3 branch %.0f + UPS %.0f)"
          % (i5 * 1000, i33 * 1000, UPS_VBUS * 1000))
    print("  polyfuse hold        %.0f mA" % (POLYFUSE_HOLD * 1000))

    fails = 0
    margin = POLYFUSE_HOLD - i5
    if margin <= 0:
        print("\n  FAIL polyfuse hold current is at or below worst-case draw")
        print("       %.0f mA hold vs %.0f mA draw -- a 750mA or 1A part gives real headroom."
              % (POLYFUSE_HOLD * 1000, i5 * 1000))
        print("       Hold is the current it will NOT trip at; derating with")
        print("       temperature makes this worse, and a closet runs warm.")
        fails += 1
    elif margin < 0.15 * POLYFUSE_HOLD:
        print("\n  WARN polyfuse margin only %.0f mA" % (margin * 1000))

    if rise > 60:
        print("  FAIL AP2114H rise %.0f C is too hot even with a pour" % rise)
        fails += 1
    else:
        print("  ok   AP2114H thermals acceptable for a closet device")

    # The fuse must not be the first thing to give way on a UPS fault. The load
    # switch limits at its own fixed current; that current passes through the
    # fuse on top of the board's own draw. If the fuse holds below that sum, a
    # shorted UPS takes down the whole bridge -- which is the exact failure the
    # switch was fitted to prevent.
    fault = i33 + SWITCH_LIMIT
    print("\n  UPS fault case       %.0f mA  (board %.0f + switch limiting at %.0f)"
          % (fault * 1000, i33 * 1000, SWITCH_LIMIT * 1000))
    margin = (POLYFUSE_HOLD - fault) / fault
    if POLYFUSE_HOLD <= fault:
        print("  FAIL polyfuse holds %.1f A, below the %.1f A fault case -- a shorted"
              % (POLYFUSE_HOLD, fault))
        print("       UPS would trip the fuse and kill the bridge, not just the port")
        fails += 1
    elif margin < 0.25:
        print("  WARN polyfuse margin over the fault case is only %.0f%%; hold derates"
              % (margin * 100))
        print("       with temperature and a closet runs warm. Tolerable, not ideal: into")
        print("       a dead short the AP2171 dissipates ~7 W in a SOT-23-5 and enters")
        print("       thermal shutdown within milliseconds, then duty-cycles, so the")
        print("       AVERAGE fault current is far below the limit and a polyfuse needs")
        print("       seconds above hold to trip. If it ever does, it self-resets.")
    else:
        print("  ok   polyfuse holds %.1f A, %.0f%% above the fault case, so the switch"
              % (POLYFUSE_HOLD, margin * 100))
        print("       limits first and the bridge survives a UPS fault")

    # The load switch's enable polarity has to agree with the pull resistor and
    # with firmware, and nothing else checks it. AP2161 and AP2171 are the same
    # package with the same pinout and differ ONLY in that polarity, so the part
    # number is the entire specification here. Fitting an AP2161 would leave the
    # switch on at power-up and make board_vbus_set(true) turn VBUS OFF: the UPS
    # would never enumerate and the power-cycle recovery would work backwards.
    fails += check_switch_polarity()
    fails += check_vbus_bulk()
    fails += check_ldo_headroom()
    fails += check_led_drive()

    if not fails:
        print("\n  ok   power budget within margins")
    return 1 if fails else 0


def check_led_drive():
    """An indicator has to light on every board, not on some of them.

    The trap is that an LED's forward voltage can sit at or above the rail
    driving it. A 0603 InGaN green is Vf = 3.3V typical at 20mA -- exactly the
    3.3V rail here -- so the current through its resistor is the difference of
    two near-equal numbers and is set by part spread, not by the resistor. D3
    was that part. Nothing in a netlist, a DRC run or a BOM check can see it.

    Vf falls at low current, so the real operating point is better than a
    fixed-Vf calculation suggests; these are the datasheet ranges at 20mA and
    the check is deliberately the pessimistic end of them.
    """
    RAIL = 3.3
    LEDS = {
        # ref: (part, Vf min, Vf max at 20mA, series resistor ohms)
        "D3": ("C2287 KT-0603Y yellow", 1.8, 2.4, 1000.0),
        "D4": ("C2286 red", 1.8, 2.4, 1000.0),
    }
    MIN_I = 0.5e-3      # below this a 0603 indicator is not readable
    bad = 0
    print()
    for ref, (part, vf_lo, vf_hi, r) in sorted(LEDS.items()):
        i_lo = max(0.0, (RAIL - vf_hi) / r)
        i_hi = max(0.0, (RAIL - vf_lo) / r)
        print("  %s %-22s Vf %.1f-%.1fV -> %.2f-%.2f mA through %.0f ohm"
              % (ref, part, vf_lo, vf_hi, i_lo * 1000, i_hi * 1000, r))
        if i_lo < MIN_I:
            print("       FAIL a worst-case part draws %.2f mA -- some boards will be dark."
                  % (i_lo * 1000))
            print("       Vf is too close to the %.1fV rail for a resistor to set the"
                  " current." % RAIL)
            bad += 1
    if not bad:
        print("  ok   both indicators light on a worst-case part")
    return bad


def check_ldo_headroom():
    """Can the regulator still make 3.3V on a bad-but-legal supply?

    Nothing checked this, and it is the single most reported failure of
    ESP32 boards: the LDO falls out of regulation on a Wi-Fi transmit burst and
    the board resets, which looks exactly like a firmware bug.

    The numbers that matter are not the nominal ones. USB is allowed to deliver
    4.75V at the port, not 5.00; a thin metre of cable at half an amp costs
    another ~200mV; the polyfuse and the trace take their share; and then the
    regulator wants its dropout on top. With the AMS1117 this board started
    with, whose dropout is 1200mV, that was
    NEGATIVE margin on a cheap cable.
    """
    VOUT = 3.3
    DROPOUT = 0.45         # AP2114H-3.3, 450mV typ at 1A -- we draw half that
    USB_MIN = 4.75         # what a compliant port may deliver
    CABLE = 0.20           # thin 1m cable at 0.5A
    FUSE = 0.06 * 0.5      # 2A 1206 PPTC, ~60 mOhm warm
    TRACE = 0.02 * 0.5

    vin = USB_MIN - CABLE - FUSE - TRACE
    need = VOUT + DROPOUT
    margin = vin - need
    print()
    print("  worst legal supply %.2f V at the port, %.2f V at the regulator"
          % (USB_MIN, vin))
    print("  needs %.2f V (%.1f V out + %.0f mV dropout)" % (need, VOUT, DROPOUT * 1000))
    if margin < 0:
        print("  FAIL %.0f mV SHORT of regulation -- Wi-Fi bursts will brown the"
              " board out" % (-margin * 1000))
        return 1
    if margin < 0.15:
        print("  WARN only %.0f mV of headroom" % (margin * 1000))
        return 0
    print("  ok   %.0f mV of headroom on a bad cable and a weak port"
          % (margin * 1000))
    return 0


def _derate(nominal_f, rail_v, rated_v):
    """Effective capacitance of an MLCC under DC bias.

    Class-II ceramics (X5R/X7R) lose capacitance as the applied voltage
    approaches the rating, and high-CV parts lose most of it: a 100uF 6.3V X5R
    measures about 48uF at 3.3V and nearer 38uF at 5V. Nothing on a schematic
    shows this, so every capacitance in this design is a NOMINAL that has to be
    derated before it means anything.

    Approximate and deliberately on the pessimistic side. Real curves depend on
    case size and vendor; for a go/no-go check, close and conservative beats
    precise and absent."""
    if not rated_v:
        return nominal_f
    return nominal_f * max(0.15, 1.0 - 0.75 * (min(rail_v / rated_v, 1.0) ** 0.8))


def _caps():
    """(ref, farads, rail volts, rated volts) for every capacitor, from the
    schematic's own table. The rating is part of the value string."""
    import re
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    spec = importlib.util.spec_from_file_location(
        "gen_schematic", os.path.join(root, "tools", "gen-schematic.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    RAIL_V = {"+3V3": 3.3, "EN": 3.3, "+5V": 5.0, "5V_RAW": 5.0, "VBUS_A": 5.0}
    out = []
    for entry in mod.DESIGN:
        ref, _lib, sym, value = entry[:4]
        if sym != "C":
            continue
        nets = entry[7] if len(entry) > 7 else {}
        rails = [RAIL_V[n] for n in nets.values() if n in RAIL_V]
        m = re.match(r"([\d.]+)(uF|nF|pF)(?:\s+([\d.]+)V)?", value)
        if not m:
            continue
        scale = {"uF": 1e-6, "nF": 1e-9, "pF": 1e-12}[m.group(2)]
        out.append((ref, float(m.group(1)) * scale,
                    max(rails) if rails else 0.0,
                    float(m.group(3)) if m.group(3) else 0.0))
    return out


def check_vbus_bulk():
    """Three things about the host port's bulk capacitance, none of which is
    visible on a schematic.

    RATING. A ceramic on a 5V rail wants headroom: USB VBUS is 5V +5%, and an
    unplugged cable's inductance rings on top of that. Below 1.25x is a fail.

    INRUSH. The AP2171 ramps its output over a controlled 0.6ms, so charging C
    costs C x 5V / 0.6ms. Past its 1.5A limit the switch current-limits on its
    own output capacitor and flags an over-current at every power-on.

    BULK. USB asks a host port for 120uF. This one is under that after
    derating, and that is a judgement rather than an oversight -- see the note
    printed below.
    """
    caps = _caps()
    bad = 0

    print()
    thin = [(r, rail, rated) for r, _f, rail, rated in caps
            if rated and rail and rated < 1.25 * rail]
    tight = [(r, rail, rated) for r, _f, rail, rated in caps
             if rated and rail and 1.25 * rail <= rated < 2.0 * rail]
    unrated = [r for r, _f, rail, rated in caps if rail and not rated]
    if unrated:
        print("  FAIL no voltage rating given for: %s" % ", ".join(unrated))
        print("       the assembler picks, and on a 5V rail that means a 6.3V part")
        bad += 1
    if thin:
        for r, rail, rated in thin:
            print("  FAIL %s is a %.1fV part on a %.1fV rail" % (r, rated, rail))
        bad += 1
    if not unrated and not thin:
        print("  ok   every capacitor is rated for its rail")
        for r, rail, rated in tight:
            print("       %s runs at %.0f%% of its rating -- see docs/BOM.md"
                  % (r, 100.0 * rail / rated))

    vbus = [(r, f, rail, rated) for r, f, rail, rated in caps if rail == 5.0]
    host = [(r, f, rail, rated) for r, f, rail, rated in vbus if r in ("C7", "C8")]
    eff = sum(_derate(f, rail, rated) for _r, f, rail, rated in host)
    nom = sum(f for _r, f, _rail, _rated in host)
    RISE, LIMIT = 0.6e-3, 1.5
    inrush = eff * 5.0 / RISE
    print("  VBUS bulk %.0f uF nominal -> %.0f uF after DC-bias derating"
          % (nom * 1e6, eff * 1e6))
    if inrush > LIMIT:
        print("  FAIL %.2f A turn-on inrush exceeds the switch's %.1f A limit"
              % (inrush, LIMIT))
        bad += 1
    else:
        print("  ok   turn-on inrush %.2f A, inside the switch's %.1f A limit"
              % (inrush, LIMIT))
    if eff < 120e-6:
        print("  NOTE %.0f uF is under the 120 uF USB asks of a host port. Accepted:"
              % (eff * 1e6))
        print("       JLC's Basic library has no 1206 above 6.3V at this value, and a")
        print("       25V part in the same package is 22uF -- LESS after derating. The")
        print("       inrush a plugged-in device draws is bounded by the switch's")
        print("       current limit, not by this capacitor, and nothing is enumerated")
        print("       at the moment of insertion for a droop to disturb.")
    return bad


def check_switch_polarity():
    import re
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sch = open(os.path.join(root, "tools", "gen-schematic.py")).read()
    hdr = open(os.path.join(root, "firmware", "components", "board",
                            "include", "board.h")).read()

    m = re.search(r'\("U4",\s*"[^"]*",\s*"(AP21\d\dW)"', sch)
    if not m:
        print("\n  FAIL cannot find U4's part number in gen-schematic.py")
        return 1
    part = m.group(1)

    # R11 pulls VBUS_EN somewhere. Which rail decides the power-up state.
    pull = re.search(r'\("R11",[^)]*?\{([^}]*)\}', sch, re.S)
    to_gnd = bool(pull and "GND" in pull.group(1))

    # Firmware drives the pin high for "on"?
    active_high_fw = "on ? 1 : 0" in open(
        os.path.join(root, "firmware", "components", "board", "board.c")).read()

    print("\n  load switch %s, R11 pulls VBUS_EN %s, firmware drives %s for on"
          % (part, "to GND" if to_gnd else "to a rail", "HIGH" if active_high_fw else "LOW"))

    if part == "AP2161W":
        print("  FAIL AP2161W has an ACTIVE LOW enable. With a pull-down the switch")
        print("       sits ON at power-up, and firmware driving the pin high to turn")
        print("       VBUS on turns it OFF. Use the AP2171W (active high).")
        return 1
    if part != "AP2171W":
        print("  WARN %s is not a part this check knows; verify its enable polarity"
              % part)
        return 0
    if not (to_gnd and active_high_fw):
        print("  FAIL AP2171W is active high, so R11 must pull VBUS_EN to GND and")
        print("       firmware must drive it high for on. One of those is not true.")
        return 1
    print("  ok   AP2171W active-high enable agrees with the pull-down and firmware")
    return 0

if __name__ == "__main__":
    sys.exit(main())
