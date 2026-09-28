#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Rev A schematic from the netlist in docs/hardware-schematic.md.

Hand-transcribing 19 nets across 30-odd parts into a GUI is where wrong boards
come from. This emits the schematic directly from the same data the spec states,
so the netlist cannot drift from the document, and kicad-cli ERC then checks it.

Connections are made with labels rather than drawn wires: electrically
identical, far more robust to generate correctly, and readable. Each label and
rail symbol sits at the end of a short wire stub running AWAY from its pin, so
two pins a grid apart never stack their labels on each other or on the part.
Parts are placed in labelled functional blocks (BLOCKS below), so the sheet
reads as a schematic rather than a netlist. The layout is generated like the
rest: do not tidy it in the GUI, change the coordinates here.

Usage: tools/gen-schematic.py
"""
import os
import json, re, sys, uuid

# Locate KiCad without hardcoding one machine's install path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ""))
import kicad_paths

SYMDIR = kicad_paths.symbols() + "/"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "hardware", "kicad")

GRID = 1.27


def uid():
    return str(uuid.uuid4())


# ---------------------------------------------------------------- symbol libs
def read_symbol(lib, name):
    """Return the raw s-expression block for one top-level symbol, and its pins."""
    path = SYMDIR + lib
    lines = open(path).read().split("\n")
    start = None
    depth = 0
    for i, l in enumerate(lines):
        if start is None:
            if re.match(r'\s*\(symbol "%s"\s*$' % re.escape(name), l):
                start = i
                depth = l.count("(") - l.count(")")
            continue
        depth += l.count("(") - l.count(")")
        if depth <= 0:
            end = i
            break
    else:
        raise SystemExit("symbol not found: %s:%s" % (lib, name))
    block = "\n".join(lines[start:end + 1])

    ext = re.search(r'\(extends "([^"]+)"\)', block)
    pins = []
    src = block
    if ext:
        # A derived symbol carries no geometry of its own, and KiCad cannot
        # resolve (extends ...) against a library that is not embedded in the
        # schematic. So embed the PARENT's definition and let the caller rename
        # it to the child's lib_id -- flattening rather than inheriting.
        parent = ext.group(1)
        src, pins = read_symbol(lib, parent)
        # Unit sub-symbols are named "<SymbolName>_<unit>_<style>" and must
        # agree with the outer symbol, so rename the parent throughout.
        src = src.replace('(symbol "%s' % parent, '(symbol "%s' % name)
    else:
        cur = None
        d = 0
        for l in block.split("\n"):
            m = re.match(r"\s*\(pin\s+(\S+)\s", l)
            if m and cur is None:
                cur = {"name": None, "num": None, "at": None}
                d = l.count("(") - l.count(")")
                continue
            if cur is not None:
                d += l.count("(") - l.count(")")
                if cur["at"] is None:
                    a = re.search(r"\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)", l)
                    if a:
                        cur["at"] = (float(a.group(1)), float(a.group(2)), float(a.group(3)))
                nm = re.search(r'\(name "([^"]*)"', l)
                nu = re.search(r'\(number "([^"]*)"', l)
                if nm and cur["name"] is None:
                    cur["name"] = nm.group(1)
                if nu and cur["num"] is None:
                    cur["num"] = nu.group(1)
                if d <= 0:
                    if cur["num"] and cur["at"]:
                        pins.append(cur)
                    cur = None
    return src, pins


# ------------------------------------------------------------------- the design
# (ref, lib, symbol, value, footprint, x, y, {pin_number: net})
# Net names beginning with a rail name become power symbols; others become labels.
RAILS = {"GND", "+5V", "+3V3"}

# Functional blocks, drawn as titled boxes: (title, x0, y0, x1, y1) in mm on A3.
# Every part's (x, y) in DESIGN falls inside one of these.
BLOCKS = [
    ("USB-C: power in and programming",       15,  15, 140, 100),
    ("USB-serial and auto-reset",             15, 105, 140, 215),
    ("Power",                                  15, 220, 140, 282),
    ("ESP32-S3 module",                      148,  15, 287, 170),
    ("UPS port: switched VBUS, USB-A host",  295,  15, 405, 125),
    ("Status LEDs and display",              295, 130, 405, 218),
    ("Expansion",                             295, 222, 405, 250),
]

# Where the two PWR_FLAGs go, inside the Power block.
PWR_FLAGS = [("+5V", 120, 238), ("GND", 120, 262)]

STUB = 2.54      # wire from a pin out to its label or rail symbol

DESIGN = [
    # --- MCU -------------------------------------------------------------
    ("U1", "RF_Module", "ESP32-S3-WROOM-1", "ESP32-S3-WROOM-1-N4",
     "RF_Module:ESP32-S3-WROOM-1", 212, 75, {
        "1": "GND", "40": "GND", "41": "GND",
        "2": "+3V3", "3": "EN", "4": "VBUS_EN", "5": "VBUS_FAULT",
        "12": "SDA", "13": "USB_DM", "14": "USB_DP", "17": "SCL",
        "24": "LED_FAULT", "25": "LED_STAT", "27": "IO0",
        "36": "UART_RX", "37": "UART_TX",
        "18": "EXP_IO10", "19": "EXP_IO11", "20": "EXP_IO12", "21": "EXP_IO13",
     }),
    # --- USB-serial ------------------------------------------------------
    ("U2", "Interface_USB", "CH340C", "CH340C", "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm",
     55, 150, {
        "1": "GND", "16": "+3V3", "4": "+3V3",
        "2": "UART_RX", "3": "UART_TX",
        "5": "PROG_DP", "6": "PROG_DM",
        "13": "DTR", "14": "RTS",
     }),
    # --- power -----------------------------------------------------------
    # AP2114H-3.3, not AMS1117-3.3. Same SOT-223, same pinout (1 GND, 2 VOUT
    # and tab, 3 VIN), and cheaper -- but the dropout is 450mV at 1A instead of
    # 1200mV, and that is the whole point.
    #
    # The AMS1117 needs Vin >= 4.5V to hold 3.3V. USB is allowed to deliver
    # 4.75V at the port, a thin metre of cable at half an amp costs another
    # 200mV, and the ESP32-S3 pulls 350mA bursts every time it transmits. That
    # is the single most reported failure of AMS1117-powered ESP32 boards:
    # brownout resets that look like firmware bugs. On a cheap cable this board
    # had NEGATIVE margin. With the AP2114H the same cable leaves 910mV.
    #
    # It is also explicitly stable with ceramic output capacitors, which the
    # AMS1117's datasheet only implies by giving a maximum ESR and no minimum.
    #
    # THE SYMBOL IS AP1117-33 because KiCad has no AP2114 symbol and AP1117-33
    # is the generic SOT-223 regulator with identical pin numbering. The VALUE
    # is what reaches the BOM.
    #
    # ORDER THE "H", NOT THE "HA". AP2114HA is the same package and also
    # stocked, with pin 1 VIN, pin 2 GND, pin 3 VOUT -- fitting it puts the
    # input rail onto ground. tools/test/test_bom.py pins the exact LCSC code.
    ("U3", "Regulator_Linear", "AP1117-33", "AP2114H-3.3",
     "Package_TO_SOT_SMD:SOT-223-3_TabPin2", 70, 245, {
        "1": "GND", "2": "+3V3", "3": "+5V",
     }),
    # AP2171W, not AP2161W. Same pinout, same package, same price bracket --
    # the ONLY difference is enable polarity, and it is the whole feature.
    # AP2161 is active LOW: with R11 pulling EN to ground the switch would sit
    # ON at power-up, and firmware driving the pin high to turn VBUS on would
    # turn it OFF. The UPS would never enumerate. AP2171 is active HIGH, which
    # is what R11's pull-down and board_vbus_set() both already assume.
    ("U4", "Power_Management", "AP2171W", "AP2171W",
     "Package_TO_SOT_SMD:SOT-23-5", 322, 40, {
        "5": "+5V", "1": "VBUS_A", "2": "GND", "4": "VBUS_EN", "3": "VBUS_FAULT",
     }),
    ("F1", "Device", "Polyfuse", "2A hold", "Fuse:Fuse_1206_3216Metric", 25, 250,
     {"1": "5V_RAW", "2": "+5V"}),
    # VBUS_EN must be defined BEFORE firmware runs. The S3's GPIO4 is high-Z out
    # of reset, and the AP2171W's EN is active high, so without this the UPS's
    # VBUS is indeterminate for the whole boot window -- and board.c's promise to
    # hold VBUS off until the USB host is listening cannot be kept.
    ("R11", "Device", "R", "100k", "Resistor_SMD:R_0402_1005Metric", 352, 40,
     {"1": "VBUS_EN", "2": "GND"}),
    # FLG is open drain. The internal pull-up only exists after gpio_config(),
    # and board_vbus_fault() reads low as a fault, so a floating line is a
    # phantom fault at boot.
    ("R12", "Device", "R", "10k", "Resistor_SMD:R_0402_1005Metric", 366, 40,
     {"1": "+3V3", "2": "VBUS_FAULT"}),
    # --- ESD -------------------------------------------------------------
    ("D1", "Power_Protection", "USBLC6-2SC6", "USBLC6-2SC6",
     "Package_TO_SOT_SMD:SOT-23-6", 105, 80, {
        "1": "PROG_DM", "6": "PROG_DM", "3": "PROG_DP", "4": "PROG_DP",
        "2": "GND", "5": "5V_RAW",
     }),
    ("D2", "Power_Protection", "USBLC6-2SC6", "USBLC6-2SC6",
     "Package_TO_SOT_SMD:SOT-23-6", 322, 105, {
        "1": "USB_DM", "6": "USB_DM", "3": "USB_DP", "4": "USB_DP",
        "2": "GND", "5": "VBUS_A",
     }),
    # --- auto-reset ------------------------------------------------------
    ("Q1", "Transistor_BJT", "MMBT3904", "MMBT3904", "Package_TO_SOT_SMD:SOT-23",
     113, 135, {"1": "Q1B", "2": "RTS", "3": "EN"}),
    ("Q2", "Transistor_BJT", "MMBT3904", "MMBT3904", "Package_TO_SOT_SMD:SOT-23",
     113, 178, {"1": "Q2B", "2": "DTR", "3": "IO0"}),
    ("R1", "Device", "R", "1k", "Resistor_SMD:R_0402_1005Metric", 95, 135,
     {"1": "DTR", "2": "Q1B"}),
    ("R2", "Device", "R", "1k", "Resistor_SMD:R_0402_1005Metric", 95, 178,
     {"1": "RTS", "2": "Q2B"}),
    # --- straps and buttons ---------------------------------------------
    ("R3", "Device", "R", "10k", "Resistor_SMD:R_0402_1005Metric", 152, 140,
     {"1": "+3V3", "2": "EN"}),
    ("R4", "Device", "R", "10k", "Resistor_SMD:R_0402_1005Metric", 215, 140,
     {"1": "+3V3", "2": "IO0"}),
    # CAPACITOR VOLTAGE RATINGS ARE PART OF THE VALUE, deliberately.
    #
    # None of these carried a rating, which left the choice to JLC's matcher --
    # and on a 5V rail the cheapest 1206 it would reach for is 6.3V, running at
    # 83% of its rating. Two separate things depend on getting this right:
    #
    #   RELIABILITY. A ceramic on a 5V rail wants 16V or better. USB VBUS is
    #   5V +5% and an unplugged cable's inductance rings on top of that.
    #
    #   CAPACITANCE. An MLCC loses capacitance under DC bias, and the loss is
    #   worst close to the rating: a 100uF 6.3V X5R is about 48uF at 3.3V and
    #   nearer 35uF at 5V, while a 25V part at 5V keeps almost all of it. Every
    #   capacitance figure in this design is therefore a NOMINAL that has to be
    #   derated before it means anything. tools/test/test_power.py does that.
    #
    # C7 is the one compromise. It is the USB host port's bulk, where effective
    # capacitance is what matters, and JLC's Basic library has no 1206 above
    # 6.3V at this value -- a 25V part in the same package is 22uF, which after
    # derating is LESS capacitance than the 6.3V part. So 6.3V stays, on the
    # rail it is least comfortable on, and is called out in docs/BOM.md.
    ("C1", "Device", "C", "1uF 16V", "Capacitor_SMD:C_0402_1005Metric", 164, 140,
     {"1": "EN", "2": "GND"}),
    # XKB TS-1187A-B-A-B (LCSC C318884, a JLC Basic part), on KiCad's own
    # footprint for it. These were on the E-Switch TL3342 land pattern with no
    # part number at all: nothing orderable was ever checked against those pads,
    # and "RESET" is not a value JLC's matcher can resolve, so the buttons would
    # have been silently dropped from the assembly order. The pin numbering is
    # the same 1,1,2,2 and the courtyard is smaller.
    ("SW1", "Switch", "SW_Push", "RESET", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
     185, 140, {"1": "EN", "2": "GND"}),
    ("SW2", "Switch", "SW_Push", "BOOT", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
     240, 140, {"1": "IO0", "2": "GND"}),
    # --- I2C -------------------------------------------------------------
    ("R5", "Device", "R", "4.7k", "Resistor_SMD:R_0402_1005Metric", 357, 152,
     {"1": "+3V3", "2": "SDA"}),
    ("R6", "Device", "R", "4.7k", "Resistor_SMD:R_0402_1005Metric", 371, 152,
     {"1": "+3V3", "2": "SCL"}),
    # FEMALE socket, not a pin header: 0.96" OLED modules ship with male pins,
    # either pre-soldered or loose in the bag. The land pattern is identical to
    # a male header, so this costs nothing.
    ("J3", "Connector_Generic", "Conn_01x04", "OLED (optional)",
     "Connector_PinSocket_2.54mm:PinSocket_1x04_P2.54mm_Vertical", 392, 152,
     {"1": "+3V3", "2": "GND", "3": "SDA", "4": "SCL"}),
    # --- LEDs ------------------------------------------------------------
    ("R7", "Device", "R", "1k", "Resistor_SMD:R_0402_1005Metric", 305, 157,
     {"1": "LED_STAT", "2": "LEDA_G"}),
    # Yellow, not green, and it is not a style choice. A 0603 InGaN green has
    # Vf = 3.3V typical at 20mA -- the same as the rail it runs from. The
    # current through R7 is then the difference of two near-equal numbers, so
    # it is set by part spread rather than by the resistor: some boards light,
    # some are dark, and no resistor value fixes it. This yellow part is
    # Vf 1.7-2.3V, giving a predictable 1.0-1.6mA across its whole spec.
    # Driving a green from the 5V rail would need a transistor; amber-for-
    # running is normal on industrial kit and costs nothing.
    ("D3", "Device", "LED", "yellow", "LED_SMD:LED_0603_1608Metric", 325, 160,
     {"2": "LEDA_G", "1": "GND"}),
    ("R8", "Device", "R", "1k", "Resistor_SMD:R_0402_1005Metric", 305, 197,
     {"1": "LED_FAULT", "2": "LEDA_R"}),
    ("D4", "Device", "LED", "red", "LED_SMD:LED_0603_1608Metric", 325, 200,
     {"2": "LEDA_R", "1": "GND"}),
    # --- connectors ------------------------------------------------------
    ("J1", "Connector", "USB_C_Receptacle_USB2.0_16P", "USB-C sink",
     "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12", 35, 45, {
        "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
        "A4": "5V_RAW", "A9": "5V_RAW", "B4": "5V_RAW", "B9": "5V_RAW",
        "A6": "PROG_DP", "B6": "PROG_DP", "A7": "PROG_DM", "B7": "PROG_DM",
        "A5": "CC1", "B5": "CC2",
     }),
    ("R9", "Device", "R", "5.1k", "Resistor_SMD:R_0402_1005Metric", 85, 40,
     {"1": "CC1", "2": "GND"}),
    ("R10", "Device", "R", "5.1k", "Resistor_SMD:R_0402_1005Metric", 97, 40,
     {"1": "CC2", "2": "GND"}),
    # XKB U231-091N-4BLRA00-S, LCSC C2880618. Surface mount right-angle, so JLC
    # places it -- the previous Wuerth part was fully through-hole and Economic
    # PCBA is SMT-only, meaning it would not have been assembled at all. Its two
    # through-hole shield legs still anchor the shell mechanically, which is what
    # a port that gets a cable pulled out of it needs. 15.5mm wide against the
    # Wuerth's 20.7, and this board's width is set by its connectors.
    ("J2", "Connector", "USB_A", "USB-A host (right-angle)",
     "Connector_USB:USB_A_Receptacle_XKB_U231-091N-4BLRA00-S", 372, 102, {
        "1": "VBUS_A", "2": "USB_DM", "3": "USB_DP", "4": "GND", "SH": "GND",
     }),
    # Expansion. Four free GPIOs and both rails, so the board can be repurposed
    # as a plain ESP32-S3 carrier -- a relay and a sensor for a garage door, say.
    # Through-hole and unpopulated by default: JLC Economic PCBA is SMT-only, so
    # these pads cost area and nothing else.
    ("J5", "Connector_Generic", "Conn_01x07", "expansion",
     "Connector_PinHeader_2.54mm:PinHeader_1x07_P2.54mm_Vertical", 392, 234, {
        "1": "+3V3", "2": "+5V", "3": "GND",
        "4": "EXP_IO10", "5": "EXP_IO11", "6": "EXP_IO12", "7": "EXP_IO13",
     }),
    # No separate 5V input header. It duplicated J5 pins 2 and 3, which already
    # bring out 5V and GND, and it sat in parallel with USB-C VBUS with no ORing
    # diode -- so powering the header while USB-C was plugged in would push one
    # supply into the other. A part that costs board area, duplicates a header
    # 30mm away and adds a way to damage a charger is not worth keeping.

    # --- decoupling ------------------------------------------------------
    ("C2", "Device", "C", "100uF 6.3V", "Capacitor_SMD:C_1206_3216Metric", 252, 45,
     {"1": "+3V3", "2": "GND"}),
    ("C3", "Device", "C", "100nF 16V", "Capacitor_SMD:C_0402_1005Metric", 272, 45,
     {"1": "+3V3", "2": "GND"}),
    ("C4", "Device", "C", "22uF 16V", "Capacitor_SMD:C_0805_2012Metric", 40, 250,
     {"1": "+5V", "2": "GND"}),
    ("C5", "Device", "C", "22uF 6.3V", "Capacitor_SMD:C_0805_2012Metric", 90, 250,
     {"1": "+3V3", "2": "GND"}),
    ("C6", "Device", "C", "100nF 16V", "Capacitor_SMD:C_0402_1005Metric", 110, 250,
     {"1": "+3V3", "2": "GND"}),
    # 100uF, not 22uF, and it is the same part as C2 so it costs no extra BOM
    # line. This is the host port's VBUS bulk. At 22uF, hot-plugging a UPS whose
    # USB interface has the spec's 10uF of input capacitance drags VBUS down to
    # 3.4V by charge sharing alone; at 100uF it holds 4.6V. The ceiling is the
    # load switch, not the droop: charging C through the AP2171's 0.6ms
    # controlled rise costs C*5V/0.6ms, which is 0.83A here and would pass 1.5A
    # -- the switch's own current limit -- somewhere above 150uF, tripping a
    # false over-current at every power-on.
    ("C7", "Device", "C", "100uF 6.3V", "Capacitor_SMD:C_1206_3216Metric", 352, 72,
     {"1": "VBUS_A", "2": "GND"}),
    ("C8", "Device", "C", "100nF 25V", "Capacitor_SMD:C_0402_1005Metric", 376, 72,
     {"1": "VBUS_A", "2": "GND"}),
    ("C9", "Device", "C", "100nF 16V", "Capacitor_SMD:C_0402_1005Metric", 25, 128,
     {"1": "+3V3", "2": "GND"}),
]

POWER_SYM = {"GND": ("power", "GND"), "+5V": ("power", "+5V"), "+3V3": ("power", "+3V3")}


def balanced(src, i):
    """The s-expression starting at src[i] == '(' , through its closing paren."""
    d = 0
    for j in range(i, len(src)):
        d += {"(": 1, ")": -1}.get(src[j], 0)
        if d == 0:
            return src[i:j + 1]
    raise ValueError("unbalanced")


def lib_property(src, name):
    """(dx, dy, angle, effects) of a library symbol's property, in sheet
    coordinates (y down), so a placed part's reference and value sit where
    KiCad itself would put them rather than on top of the body."""
    m = re.search(r'\(property "%s" "[^"]*"\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)' % name, src)
    if not m:
        return 0.0, 0.0, 0, "(effects (font (size 1.27 1.27)))"
    e = src.find("(effects", m.end())
    eff = balanced(src, e) if e >= 0 else "(effects (font (size 1.27 1.27)))"
    eff = re.sub(r"\s*\(hide yes\)", "", eff)
    return float(m.group(1)), -float(m.group(2)), int(float(m.group(3))), eff


# A pin's (at) angle is the direction from its connection point INTO the body,
# in library coordinates (y up). Its outward direction on the sheet (y down):
OUTWARD = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}
# Label angle that makes the text run away from the pin.
LABEL_ANGLE = {(-1, 0): 180, (1, 0): 0, (0, -1): 90, (0, 1): 270}
# Rail symbol rotation so it points away from the pin. KiCad rotates
# counter-clockwise as drawn; GND hangs down at 0, +3V3/+5V point up at 0.
GND_ROT = {(0, 1): 0, (0, -1): 180, (1, 0): 90, (-1, 0): 270}
VCC_ROT = {(0, -1): 0, (0, 1): 180, (-1, 0): 90, (1, 0): 270}


def rot(px, py, ang):
    if ang == 0:
        return px, py
    if ang == 90:
        return -py, px
    if ang == 180:
        return -px, -py
    return py, -px


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    libcache = {}
    used = {}
    for ref, lib, sym, val, fp, x, y, nets in DESIGN:
        key = (lib, sym)
        if key not in libcache:
            libcache[key] = read_symbol(lib + ".kicad_sym", sym)
        used[key] = True
    for rail, (lib, sym) in POWER_SYM.items():
        key = (lib, sym)
        if key not in libcache:
            libcache[key] = read_symbol(lib + ".kicad_sym", sym)
    key = ("power", "PWR_FLAG")
    libcache[key] = read_symbol("power.kicad_sym", "PWR_FLAG")

    out = []
    out.append("(kicad_sch")
    out.append("\t(version 20250114)")
    out.append('\t(generator "ups-adaptor gen-schematic.py")')
    out.append('\t(generator_version "9.0")')
    out.append('\t(uuid "%s")' % uid())
    out.append('\t(paper "A3")')
    out.append("\t(title_block")
    out.append('\t\t(title "UPS to ESP32 Module, Rev A")')
    out.append('\t\t(company "GPL-3.0-or-later")')
    out.append('\t\t(comment 1 "Generated by tools/gen-schematic.py from docs/hardware-schematic.md")')
    out.append('\t\t(comment 2 "Laid out in blocks by the generator: change it there, not in the GUI.")')
    out.append("\t)")

    # embedded symbol definitions
    out.append("\t(lib_symbols")
    for (lib, sym), (src, pins) in libcache.items():
        body = src.split("\n")
        body[0] = re.sub(r'\(symbol "([^"]+)"', '(symbol "%s:%s"' % (lib, sym), body[0])
        out.append("\n".join("\t" + b for b in body))
    out.append("\t)")

    labels = []
    powersyms = []
    counter = {'pwr': 0, 'flg': 0}

    for ref, lib, sym, val, fp, x, y, nets in DESIGN:
        src, pins = libcache[(lib, sym)]
        out.append("\t(symbol")
        out.append('\t\t(lib_id "%s:%s")' % (lib, sym))
        out.append("\t\t(at %g %g 0)" % (x, y))
        out.append("\t\t(unit 1)")
        out.append("\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)")
        out.append('\t\t(uuid "%s")' % uid())
        for prop, text in (("Reference", ref), ("Value", val)):
            dx, dy, pa, eff = lib_property(src, prop)
            out.append('\t\t(property "%s" "%s" (at %g %g %d)%s)'
                       % (prop, text, x + dx, y + dy, pa, eff))
        out.append('\t\t(property "Footprint" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))'
                   % (fp, x, y))
        out.append('\t\t(property "Datasheet" "~" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))'
                   % (x, y))
        for p in pins:
            out.append('\t\t(pin "%s" (uuid "%s"))' % (p["num"], uid()))
        out.append("\t\t(instances")
        out.append('\t\t\t(project "ups-adaptor"')
        out.append('\t\t\t\t(path "/%s"' % SHEET_UUID)
        out.append('\t\t\t\t\t(reference "%s")(unit 1)' % ref)
        out.append("\t\t\t\t)")
        out.append("\t\t\t)")
        out.append("\t\t)")
        out.append("\t)")

        done = set()
        for p in pins:
            net = nets.get(p["num"])
            px, py, pang = p["at"]
            ax, ay = round(x + px, 2), round(y - py, 2)
            # Stacked pins (the module's several GND pads, say) share one
            # point; one stub and one label serve them all.
            if (ax, ay) in done:
                continue
            done.add((ax, ay))
            if net is None:
                out.append('\t(no_connect (at %g %g) (uuid "%s"))' % (ax, ay, uid()))
                continue
            ox, oy = OUTWARD[int(pang) % 360]
            ex, ey = round(ax + ox * STUB, 2), round(ay + oy * STUB, 2)
            out.append('\t(wire (pts (xy %g %g) (xy %g %g)) (stroke (width 0) (type default)) (uuid "%s"))'
                       % (ax, ay, ex, ey, uid()))
            if net in POWER_SYM:
                powersyms.append((net, ex, ey, (ox, oy)))
            else:
                labels.append((net, ex, ey, (ox, oy)))

    for net, ax, ay, o in labels:
        ang = LABEL_ANGLE[o]
        out.append('\t(global_label "%s"' % net)
        out.append("\t\t(shape bidirectional)")
        out.append("\t\t(at %g %g %g)" % (ax, ay, ang))
        out.append('\t\t(effects (font (size 1.27 1.27)) (justify %s))'
                   % ("left" if ang in (0, 90) else "right"))
        out.append('\t\t(uuid "%s")' % uid())
        out.append("\t)")

    shown, free_text = [], []
    for net, ax, ay, o in powersyms:
        lib, sym = POWER_SYM[net]
        ang = (GND_ROT if net == "GND" else VCC_ROT)[o]
        counter['pwr'] += 1
        pref = "#PWR%02d" % counter['pwr']
        out.append("\t(symbol")
        out.append('\t\t(lib_id "%s:%s")' % (lib, sym))
        out.append("\t\t(at %g %g %d)" % (ax, ay, ang))
        out.append("\t\t(unit 1)")
        out.append("\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)")
        out.append('\t\t(uuid "%s")' % uid())
        out.append('\t\t(property "Reference" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))' % (pref, ax, ay))
        # The rail's NAME, visible: an arrow alone does not say +3V3 or +5V.
        # The library puts it beyond the symbol's tip; rotate that offset with
        # the symbol and keep the text itself horizontal.
        vdx, vdy, _, _ = lib_property(libcache[(lib, sym)][0], "Value")
        rx, ry = rot(vdx, -vdy, ang)
        # Centred is KiCad's default and has no keyword: "(justify center)"
        # makes the whole schematic fail to load.
        just = " (justify right)" if rx < -0.1 else " (justify left)" if rx > 0.1 else ""
        # One name per rail per spot: two adjacent GND pins print "GNDGND".
        tx, ty = ax + rx, ay - ry
        hide = any(n == net and abs(nx - tx) < 6 and abs(ny - ty) < 3 for n, nx, ny in shown)
        if not hide:
            shown.append((net, tx, ty))
        # A SIDEWAYS rail's own Value text follows the symbol's rotation and
        # lands on the arrow. So it is hidden, and the name is drawn as plain
        # text just past the tip instead -- visual only; the symbol still makes
        # the connection. Upright and upside-down rails keep their own text,
        # unrotated (a stored 180 renders it upside down).
        sideways = ang in (90, 270)
        out.append('\t\t(property "Value" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))%s%s))'
                   % (net, tx, ty, just, " (hide yes)" if (hide or sideways) else ""))
        if sideways and not hide:
            ox, oy = o
            free_text.append((net, ax + ox * 3.4, ay, "left" if ox > 0 else "right"))
        out.append('\t\t(pin "1" (uuid "%s"))' % uid())
        out.append("\t\t(instances")
        out.append('\t\t\t(project "ups-adaptor"')
        out.append('\t\t\t\t(path "/%s"' % SHEET_UUID)
        out.append('\t\t\t\t\t(reference "%s")(unit 1)' % pref)
        out.append("\t\t\t\t)")
        out.append("\t\t\t)")
        out.append("\t\t)")
        out.append("\t)")

    # PWR_FLAGs so ERC sees the rails as driven
    # No flag on +3V3: U3's VO already drives it, and a second power output on
    # the same net is exactly what ERC is right to complain about. +5V and GND
    # are fed only by passive connector and fuse pins, so they do need one.
    # Each PWR_FLAG sits at the end of a short wire from a named rail symbol,
    # so it reads as "this rail is driven" rather than a stray flag.
    for i, (net, rx0, fy) in enumerate(PWR_FLAGS):
        fx = rx0 + 5.08
        out.append('\t(wire (pts (xy %g %g) (xy %g %g)) (stroke (width 0) (type default)) (uuid "%s"))'
                   % (rx0, fy, fx, fy, uid()))
        out.append("\t(symbol")
        out.append('\t\t(lib_id "power:PWR_FLAG")')
        out.append("\t\t(at %g %g 0)" % (fx, fy))
        out.append("\t\t(unit 1)")
        out.append("\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)")
        out.append('\t\t(uuid "%s")' % uid())
        counter['flg'] += 1
        fref = "#FLG%02d" % counter['flg']
        out.append('\t\t(property "Reference" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))' % (fref, fx, fy))
        out.append('\t\t(property "Value" "PWR_FLAG" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))' % (fx, fy))
        out.append('\t\t(pin "1" (uuid "%s"))' % uid())
        out.append("\t\t(instances")
        out.append('\t\t\t(project "ups-adaptor"')
        out.append('\t\t\t\t(path "/%s"' % SHEET_UUID)
        out.append('\t\t\t\t\t(reference "%s")(unit 1)' % fref)
        out.append("\t\t\t\t)")
        out.append("\t\t\t)")
        out.append("\t\t)")
        out.append("\t)")
        lib, sym = POWER_SYM[net]
        vdx, vdy, _, _ = lib_property(libcache[(lib, sym)][0], "Value")
        out.append("\t(symbol")
        out.append('\t\t(lib_id "%s:%s")' % (lib, sym))
        out.append("\t\t(at %g %g 0)" % (rx0, fy))
        out.append("\t\t(unit 1)")
        out.append("\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)")
        out.append('\t\t(uuid "%s")' % uid())
        counter['pwr'] += 1
        pref2 = "#PWR%02d" % counter['pwr']
        out.append('\t\t(property "Reference" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))(hide yes)))' % (pref2, rx0, fy))
        out.append('\t\t(property "Value" "%s" (at %g %g 0)(effects (font (size 1.27 1.27))))' % (net, rx0 + vdx, fy + vdy))
        out.append('\t\t(pin "1" (uuid "%s"))' % uid())
        out.append("\t\t(instances")
        out.append('\t\t\t(project "ups-adaptor"')
        out.append('\t\t\t\t(path "/%s"' % SHEET_UUID)
        out.append('\t\t\t\t\t(reference "%s")(unit 1)' % pref2)
        out.append("\t\t\t\t)")
        out.append("\t\t\t)")
        out.append("\t\t)")
        out.append("\t)")

    for net, tx, ty, just in free_text:
        out.append('\t(text "%s" (exclude_from_sim no) (at %g %g 0) '
                   '(effects (font (size 1.27 1.27)) (justify %s)) (uuid "%s"))' % (net, tx, ty, just, uid()))

    for title, x0, y0, x1, y1 in BLOCKS:
        out.append('\t(rectangle (start %g %g) (end %g %g) (stroke (width 0.25) (type dash)) (fill (type none)) (uuid "%s"))'
                   % (x0, y0, x1, y1, uid()))
        out.append('\t(text "%s" (exclude_from_sim no) (at %g %g 0) '
                   '(effects (font (size 2 2) (thickness 0.4) bold) (justify left bottom)) (uuid "%s"))'
                   % (title, x0 + 1.5, y0 + 4.5, uid()))

    out.append('\t(sheet_instances\n\t\t(path "/" (page "1"))\n\t)')
    out.append(")")

    path = os.path.join(OUTDIR, "ups-adaptor.kicad_sch")
    open(path, "w").write("\n".join(out) + "\n")

    # MERGE into the project file, do not replace it. It also holds the board's
    # design rules and net classes (tools/gen-pcb.py writes them); replacing it
    # left a bare file that only a full board build repaired, so regenerating
    # the schematic alone silently dropped every rule.
    pro = os.path.join(OUTDIR, "ups-adaptor.kicad_pro")
    try:
        cfg = json.load(open(pro))
    except (OSError, ValueError):
        cfg = {"board": {}, "meta": {"filename": "ups-adaptor.kicad_pro", "version": 1},
               "text_variables": {}}
    cfg["sheets"] = [[SHEET_UUID, "Root"]]
    json.dump(cfg, open(pro, "w"), indent=2)
    open(pro, "a").write("\n")

    print("wrote %s" % path)
    print("%d components, %d labels, %d power symbols" % (len(DESIGN), len(labels), len(powersyms)))
    export_docs(path)


def export_docs(sch):
    """docs/schematic.pdf and docs/img/schematic.svg, for people without KiCad,
    and docs/schematic.sha256 naming the schematic they were drawn from.
    Written on every run, so they cannot fall behind the design; and
    tools/test/test_schematic_docs.py fails if the schematic has changed
    since -- which is also what a hand edit in the GUI would do."""
    import hashlib, shutil, subprocess, tempfile
    kc = kicad_paths.cli()
    docs = os.path.join(ROOT, "docs")
    quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([kc, "sch", "export", "pdf", "-o", os.path.join(docs, "schematic.pdf"), sch],
                   check=True, **quiet)
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([kc, "sch", "export", "svg", "-o", td, sch], check=True, **quiet)
        svg = [f for f in os.listdir(td) if f.endswith(".svg")][0]
        shutil.copy(os.path.join(td, svg), os.path.join(docs, "img", "schematic.svg"))
    digest = hashlib.sha256(open(sch, "rb").read()).hexdigest()
    open(os.path.join(docs, "schematic.sha256"), "w").write(digest + "\n")
    print("exported docs/schematic.pdf, docs/img/schematic.svg")


SHEET_UUID = uid()

if __name__ == "__main__":
    main()
