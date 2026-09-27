#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assert the generated schematic's netlist says what the design intends.

These are the connections a wrong board would get wrong silently. ERC cannot
catch any of them: a crossed UART or a mis-wired transistor is electrically
legal and completely broken.
"""
import os
import sys, re, subprocess, sys, tempfile

# Locate KiCad without hardcoding one machine's install path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ""))
import kicad_paths

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KC = kicad_paths.cli()
SCH = os.path.join(ROOT, "hardware", "kicad", "ups-adaptor.kicad_sch")

# net -> exact set of ref.pin that must be on it
EXPECT = {
    "UART_TX":    {"U1.37", "U2.3"},       # S3 TXD0 -> CH340C RXD (crossed)
    "UART_RX":    {"U1.36", "U2.2"},       # CH340C TXD -> S3 RXD0
    "EN":         {"U1.3", "Q1.3", "R3.2", "C1.1", "SW1.1"},
    "IO0":        {"U1.27", "Q2.3", "R4.2", "SW2.1"},
    "DTR":        {"U2.13", "Q2.2", "R1.1"},
    "RTS":        {"U2.14", "Q1.2", "R2.1"},
    "VBUS_EN":    {"U1.4", "U4.4", "R11.1"},    # R11: 100k to GND, boot-state
                                                # hold. See test_pinmap.py.
    "VBUS_FAULT": {"U1.5", "U4.3", "R12.2"},    # R12: 10k to 3V3, open-drain FLG
    "USB_DM":     {"U1.13", "D2.1", "D2.6", "J2.2"},
    "USB_DP":     {"U1.14", "D2.3", "D2.4", "J2.3"},
    "CC1":        {"J1.A5", "R9.1"},
    "CC2":        {"J1.B5", "R10.1"},
    "SDA":        {"U1.12", "R5.2", "J3.3"},
    "SCL":        {"U1.17", "R6.2", "J3.4"},
    # Expansion GPIOs must reach the header and nothing else.
    "EXP_IO10":   {"U1.18", "J5.4"},
    "EXP_IO11":   {"U1.19", "J5.5"},
    "EXP_IO12":   {"U1.20", "J5.6"},
    "EXP_IO13":   {"U1.21", "J5.7"},
}

def parse(path):
    t = open(path).read()
    nets = {}
    blocks = re.split(r"\n\t\t\(net\n", t)
    for b in blocks[1:]:
        nm = re.search(r'\(name "([^"]+)"\)', b)
        if not nm:
            continue
        nodes = re.findall(r'\(ref "([^"]+)"\)\s*\n\s*\(pin "([^"]+)"\)', b)
        nets[nm.group(1)] = {"%s.%s" % (r, p) for r, p in nodes}
    return nets

def main():
    tmp = tempfile.mktemp(suffix=".net")
    subprocess.run([KC, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "-o", tmp, SCH], capture_output=True, text=True)
    if not os.path.exists(tmp):
        print("  netlist export failed"); return 1
    nets = parse(tmp)

    fails = 0
    for name, want in sorted(EXPECT.items()):
        got = nets.get(name)
        if got is None:
            print("  FAIL %-12s net absent" % name); fails += 1; continue
        if got != want:
            print("  FAIL %-12s missing=%s unexpected=%s"
                  % (name, sorted(want - got) or "-", sorted(got - want) or "-"))
            fails += 1
        else:
            print("  ok   %-12s %d nodes" % (name, len(got)))

    # A net with a single node is a wiring mistake, not a design.
    for name, nodes in sorted(nets.items()):
        if len(nodes) < 2 and not name.startswith("unconnected-"):
            print("  FAIL %-12s only %d node -- floating" % (name, len(nodes)))
            fails += 1

    # The S3's USB pins must reach the host connector, not the programming one.
    if nets.get("USB_DM", set()) & {"U2.6", "J1.A7"}:
        print("  FAIL USB host pins are tied to the programming port"); fails += 1

    print("  %d nets total" % len(nets))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
