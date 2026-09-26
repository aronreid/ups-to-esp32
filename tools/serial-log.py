#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Timestamped serial logger.

Survives what breaks a plain `cat`: the board resetting, the cable being
pulled, the port changing name between a CH340 (cu.wchusbserial*) and macOS's
own CDC driver (cu.usbmodem*).

Every line carries a wall-clock stamp, so a GAP is evidence too -- a silent
minute means the board was not talking, which is what you are usually hunting.

    tools/serial-log.py &          # appends to /tmp/ups-serial.log
    tools/serial-check.py          # summarise it
"""
import glob, os, time, datetime

LOG = "/tmp/ups-serial.log"
PATTERNS = ("/dev/cu.usbmodem*", "/dev/cu.wchusbserial*")

def port():
    for p in PATTERNS:
        m = sorted(glob.glob(p))
        if m:
            return m[0]
    return None

def stamp():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

def note(f, msg):
    f.write("%s  === %s ===\n" % (stamp(), msg))
    f.flush()

def main():
    import serial
    f = open(LOG, "a", buffering=1)
    note(f, "logger started")
    cur = None
    while True:
        p = port()
        if not p:
            if cur is not None:
                note(f, "port disappeared (board unplugged or reset)")
                cur = None
            time.sleep(2)
            continue
        try:
            s = serial.Serial(p, 115200, timeout=1)
            note(f, "attached to %s" % p)
            cur = p
            buf = b""
            while True:
                d = s.read(4096)
                if d:
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        txt = line.decode("utf-8", "replace").rstrip("\r")
                        if txt:
                            f.write("%s  %s\n" % (stamp(), txt))
                if not os.path.exists(p):
                    raise IOError("port vanished")
        except Exception as e:
            note(f, "disconnected: %s" % e)
            cur = None
            time.sleep(2)

main()
