#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression: what the ESP32 reads must match what a known-good host read.

Two independent USB stacks pulling identical bytes is the strongest evidence
that acquisition is correct. If this ever diverges, the bug is in the firmware's
USB path, not in interpretation.
"""
import os, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAP = os.path.join(ROOT, "captures")

def main():
    host = glob.glob(os.path.join(CAP, "051d-0002-2*", "report-descriptor.bin"))
    esp = os.path.join(CAP, "051d-0002-esp32-m0b", "report-descriptor.bin")
    if not host or not os.path.exists(esp):
        print("  SKIP no matched capture pair")
        return 0
    a, b = open(host[0], "rb").read(), open(esp, "rb").read()
    if a == b:
        print("  ok   host and ESP32 descriptors identical (%d bytes)" % len(a))
        return 0
    print("  FAIL descriptors differ: host %d bytes, esp %d bytes" % (len(a), len(b)))
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print("       first difference at byte %d: host=%02x esp=%02x" % (i, x, y))
            break
    return 1

if __name__ == "__main__":
    sys.exit(main())
