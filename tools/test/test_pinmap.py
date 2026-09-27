#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does the hardware match the firmware, and is every pin safe before boot?

Firmware is reflashable; copper is not. So the questions worth asking before fab
are not about features, they are:

  1. Does each BOARD_PIN_* in board.h land on the net the schematic wires?
     A pin map that disagrees with the board is a dead board, and nothing else
     in the harness compares the two.

  2. Is each pin in a defined state BEFORE firmware configures it? Every ESP32-S3
     GPIO is high-Z out of reset. Anything whose safe state depends on a level
     during that window needs a resistor, because firmware cannot act in a window
     that ends before it runs.

  3. Do the BOARD_HAS_* capability flags match what is actually fitted?
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
KC = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
SYMDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/"
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")
BOARD_H = os.path.join(ROOT, "firmware", "components", "board", "include", "board.h")

# BOARD_PIN_* -> the net that pin must be on in the schematic.
EXPECT_NET = {
    "BOARD_PIN_VBUS_EN":     "VBUS_EN",
    "BOARD_PIN_VBUS_FAULT":  "VBUS_FAULT",
    "BOARD_PIN_LED_STATUS":  "LED_STAT",
    "BOARD_PIN_LED_FAULT":   "LED_FAULT",
    "BOARD_PIN_I2C_SDA":     "SDA",
    "BOARD_PIN_I2C_SCL":     "SCL",
    "BOARD_PIN_FACTORY_BTN": "IO0",
}

# Nets whose level must be defined before firmware runs, and which way.
BOOT_STATE = [
    ("VBUS_EN", "GND", "VBUS to the UPS must be OFF until firmware asserts it; "
                       "EN is active high and GPIO4 is high-Z out of reset"),
    ("VBUS_FAULT", "+3V3", "FLG is open drain and board_vbus_fault() reads low as "
                           "a fault, so an unpulled line is a phantom fault at boot"),
    ("IO0", "+3V3", "GPIO0 is a strapping pin: low at reset means download mode"),
    ("EN", "+3V3", "the S3's reset pin must idle high"),
]

# Strapping pins that must be left unconnected.
STRAPPING_UNCONNECTED = {3: "IO3", 45: "IO45", 46: "IO46"}


def board_h_pins():
    """GPIO numbers from the Rev A branch of board.h."""
    text = open(BOARD_H).read()
    start = text.index("defined(CONFIG_UPSA_BOARD_REV_A)")
    # The branch ends at the next board (#elif) or at the no-board #else.
    end = re.compile(r"^#el(?:if|se)\b", re.M).search(text, start).start()
    body = text[start:end]
    pins, flags = {}, {}
    for m in re.finditer(r"#define\s+(BOARD_PIN_\w+)\s+GPIO_NUM_(\w+)", body):
        v = m.group(2)
        pins[m.group(1)] = None if v == "NC" else int(v)
    for m in re.finditer(r"#define\s+(BOARD_HAS_\w+)\s+(\d+)", body):
        flags[m.group(1)] = int(m.group(2))
    return pins, flags


def esp32_pin_to_gpio():
    """Map ESP32-S3-WROOM-1 symbol pin numbers to GPIO numbers."""
    txt = open(SYMDIR + "RF_Module.kicad_sym").read()
    i = txt.index('(symbol "ESP32-S3-WROOM-1"')
    depth, j = 0, i
    while j < len(txt):
        if txt[j] == "(":
            depth += 1
        elif txt[j] == ")":
            depth -= 1
            if depth == 0:
                break
        j += 1
    block = txt[i:j]
    out = {}
    for m in re.finditer(r'\(name "([^"]+)".*?\(number "([^"]+)"', block, re.S):
        nm, num = m.group(1), m.group(2)
        g = re.fullmatch(r"IO(\d+)", nm)
        if g:
            out[num] = int(g.group(1))
        elif nm == "USB_D-":
            out[num] = 19
        elif nm == "USB_D+":
            out[num] = 20
        elif nm == "TXD0":
            out[num] = 43
        elif nm == "RXD0":
            out[num] = 44
    return out


def netlist():
    tmp = tempfile.mktemp(suffix=".net")
    subprocess.run([KC, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "-o", tmp, SCH], capture_output=True, text=True)
    text = open(tmp).read()
    nets = {}
    for b in re.split(r"\n\t\t\(net\n", text)[1:]:
        nm = re.search(r'\(name "([^"]+)"\)', b)
        if not nm:
            continue
        nets[nm.group(1)] = re.findall(r'\(ref "([^"]+)"\)\s*\n\s*\(pin "([^"]+)"\)', b)
    return nets


def resistor_values():
    """ref -> value, so a pull resistor can be named in the output."""
    out = {}
    for m in re.finditer(r'\(symbol\s*\n\s*\(lib_id "Device:R"\).*?'
                         r'\(property "Reference" "(R\d+)".*?\(property "Value" "([^"]+)"',
                         open(SCH).read(), re.S):
        out[m.group(1)] = m.group(2)
    return out


def main():
    pins, flags = board_h_pins()
    pin2gpio = esp32_pin_to_gpio()
    nets = netlist()
    rvals = resistor_values()
    gpio_net = {}
    for net, nodes in nets.items():
        for ref, pin in nodes:
            if ref == "U1" and pin in pin2gpio:
                gpio_net[pin2gpio[pin]] = net

    fails = 0

    # 1. firmware pin map vs schematic
    for macro, want_net in sorted(EXPECT_NET.items()):
        gpio = pins.get(macro)
        if gpio is None:
            print("  FAIL %s is NC on Rev A but the schematic expects %s" % (macro, want_net))
            fails += 1
            continue
        got = gpio_net.get(gpio)
        if got == want_net:
            print("  ok   %-22s GPIO%-2d -> %s" % (macro, gpio, got))
        else:
            print("  FAIL %-22s GPIO%-2d is on %r, firmware expects %r"
                  % (macro, gpio, got, want_net))
            fails += 1

    # 2. boot-state safety
    print()
    for net, rail, why in BOOT_STATE:
        nodes = nets.get(net, [])
        puller = None
        for ref, pin in nodes:
            if ref.startswith("R"):
                other = [n for n in nets.get(rail, []) if n[0] == ref]
                if other:
                    puller = ref
                    break
        if puller:
            print("  ok   %-11s pulled to %-5s by %s (%s)"
                  % (net, rail, puller, rvals.get(puller, "?")))
        else:
            print("  FAIL %-11s has no resistor to %s" % (net, rail))
            print("       %s" % why)
            fails += 1

    # 3. strapping pins must be unwired
    print()
    for gpio, name in sorted(STRAPPING_UNCONNECTED.items()):
        net = gpio_net.get(gpio)
        if net is None or net.startswith("unconnected-"):
            print("  ok   %-5s (GPIO%d) left unconnected" % (name, gpio))
        else:
            print("  FAIL %-5s (GPIO%d) is wired to %s -- strapping pin" % (name, gpio, net))
            fails += 1

    # 4. capability flags vs what is fitted
    print()
    has_switch = any(r == "U4" for net in nets.values() for r, _ in net)
    if bool(flags.get("BOARD_HAS_VBUS_SWITCH")) == has_switch:
        print("  ok   BOARD_HAS_VBUS_SWITCH agrees with the load switch being fitted")
    else:
        print("  FAIL BOARD_HAS_VBUS_SWITCH=%s but load switch fitted=%s"
              % (flags.get("BOARD_HAS_VBUS_SWITCH"), has_switch))
        fails += 1

    has_flag = "VBUS_FAULT" in nets
    if bool(flags.get("BOARD_HAS_VBUS_FAULT")) == has_flag:
        print("  ok   BOARD_HAS_VBUS_FAULT agrees with a fault net existing")
    else:
        print("  FAIL BOARD_HAS_VBUS_FAULT=%s but VBUS_FAULT net exists=%s"
              % (flags.get("BOARD_HAS_VBUS_FAULT"), has_flag))
        fails += 1

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
