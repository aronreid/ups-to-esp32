#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a serial log: is the board misbehaving, and why.

Answers in the order that matters: is it restarting, why, what is it
complaining about, and is the UPS talking.

The FATAL list below is deliberately long. Its first version held only abort()
and Guru Meditation, and it printed "panics: none" while the board reset every
five seconds on a stack overflow. A monitor that misses the fault is worse than
no monitor, because it is believed.
"""
import re, sys, collections, datetime

LOG = "/tmp/ups-serial.log"
lines = open(LOG, errors="replace").read().splitlines()
if not lines:
    print("log is empty"); sys.exit()

def ts(l):
    m = re.match(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)", l)
    return m.group(1) if m else None

first, last = next((ts(l) for l in lines if ts(l)), "?"), next((ts(l) for l in reversed(lines) if ts(l)), "?")
print("window      : %s  ->  %s   (%d lines)" % (first, last, len(lines)))

boots = [l for l in lines if "Calling app_main" in l]
rsts  = [m.group(1) for l in lines for m in [re.search(r"rst:0x[0-9a-f]+ \(([A-Z_]+)\)", l)] if m]
print("boots       : %d" % len(boots))
if rsts:
    for r, n in collections.Counter(rsts).most_common():
        print("  reset     : %-22s x%d" % (r, n))
if len(boots) > 1:
    print("  ** RESTARTING ** last boot at", ts(boots[-1]))

# Every way this firmware has actually been seen to die. The first version of
# this list held only abort() and Guru Meditation, and reported "panics: none"
# while the board was resetting every five seconds on a stack overflow -- a
# monitor that misses the fault is worse than no monitor, because it is
# believed.
FATAL = ("abort()", "Guru Meditation", "Brownout", "stack overflow",
         "StoreProhibited", "LoadProhibited", "IllegalInstruction",
         "InstrFetchProhibited", "Interrupt wdt", "Task watchdog",
         "assert failed", "CORRUPT HEAP", "Stack canary")
panics = [l for l in lines if any(f in l for f in FATAL)]
if panics:
    print("panics      : %d" % len(panics))
    for p in panics[-3:]:
        print("   ", p[:150])
    for i, l in enumerate(lines):
        if any(f in l for f in FATAL):
            for c in lines[max(0, i-8):i+1]:
                if any(k in c for k in ("ESP_ERROR_CHECK", "assert", "file:",
                                        "expression", "in task", "ERROR***")):
                    print("   cause:", c[:150])
            break
else:
    print("panics      : none")

errs = [l for l in lines if re.search(r"\bE \(\d+\)", l)]
warns = [l for l in lines if re.search(r"\bW \(\d+\)", l)]
def squash(ls):
    c = collections.Counter(re.sub(r"\(\d+\)", "()", l.split("  ", 1)[-1]) for l in ls)
    return c.most_common(6)
print("errors      : %d" % len(errs))
for m, n in squash(errs): print("   x%-4d %s" % (n, m[:120]))
print("warnings    : %d" % len(warns))
for m, n in squash(warns): print("   x%-4d %s" % (n, m[:120]))

ups = [l for l in lines if "ups_hid:" in l and ("batt" in l or ":0" in l)]
print("ups         : %s" % (ups[-1].split("  ",1)[-1][:110] if ups else "no readings seen"))
ota = [l for l in lines if "ota:" in l]
print("ota         : %s" % (ota[-1].split("  ",1)[-1][:110] if ota else "nothing yet"))

gaps = []
prev = None
for l in lines:
    t = ts(l)
    if not t: continue
    d = datetime.datetime.strptime(t, "%Y-%m-%d %H:%M:%S")
    if prev and (d - prev).total_seconds() > 60:
        gaps.append((prev.strftime("%H:%M:%S"), t[-8:], int((d - prev).total_seconds())))
    prev = d
print("silent gaps : %s" % (", ".join("%s->%s (%ds)" % g for g in gaps[-4:]) if gaps else "none over 60s"))
