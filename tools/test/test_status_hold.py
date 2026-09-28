#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""status_hold(): the false-OFF fix, compiled and run on the host.

A UPS's status flags have been seen to read back all-zero for a minute or two
after a self-test (a CyberPower EC850LCD, 2026-09-27, docs/rev-b.md). Taken at
face value that maps to ups.status OFF -- the same glitch that made upsmon
shut hosts down on APC RS units (NUT #2104). status_hold() (ups_hid's
status_hold.h) holds the last known status through a gap like that instead,
and never gives up on its own; ups_hid.c's ups_task is what forces it through,
with a VBUS recovery, once the gap has run past STATUS_HOLD_US. This only
tests the pure decision, not that timeout -- there is no UPS on this host to
recover.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
INC = os.path.join(ROOT, "firmware", "components", "ups_hid", "include")

TESTS = r'''
#include <stdio.h>
#include "status_hold.h"

static int fails = 0;
#define CHECK(name, cond) do { if (cond) printf("  ok   %s\n", name); \
    else { printf("  FAIL %s\n", name); fails++; } } while (0)

#define SEC 1000000LL
#define OL 1u
#define OB 2u

int main(void)
{
    /* ---- ordinary reads: never held ---- */
    int64_t hold = 0;
    CHECK("a real status passes straight through", status_hold(OL, OL, &hold, 0) == OL);
    CHECK("passing through starts no hold", hold == 0);
    CHECK("a changed real status passes straight through",
          status_hold(OB, OL, &hold, 10 * SEC) == OB);
    CHECK("changing status starts no hold", hold == 0);

    /* ---- the glitch: prev real, raw suddenly zero ---- */
    hold = 0;
    CHECK("all-zero after a real status is held, not reported",
          status_hold(0, OL, &hold, 100 * SEC) == OL);
    CHECK("holding starts the clock", hold == 100 * SEC);
    CHECK("still zero a moment later: still held, clock does not restart",
          status_hold(0, OL, &hold, 101 * SEC) == OL);
    CHECK("clock unchanged on a repeat zero read", hold == 100 * SEC);
    CHECK("still held two minutes in (< the observed ~2 min glitch is not the "
          "cutoff -- status_hold() never expires it on its own)",
          status_hold(0, OL, &hold, 220 * SEC) == OL);
    CHECK("still no expiry: that is the CALLER's job (ups_task, STATUS_HOLD_US)",
          hold == 100 * SEC);

    /* ---- recovery: a real status arrives while held ---- */
    CHECK("a real status ends the hold, whatever it is",
          status_hold(OB, OL, &hold, 250 * SEC) == OB);
    CHECK("recovering clears the clock", hold == 0);

    /* ---- already unknown: nothing to hold ---- */
    hold = 0;
    CHECK("zero after zero (never had a real status) passes straight through",
          status_hold(0, 0, &hold, 5 * SEC) == 0);
    CHECK("no hold starts when there was nothing to hold", hold == 0);

    /* ---- a fresh hold after a fresh real status ---- */
    hold = 0;
    status_hold(OL, 0, &hold, 0);                    /* UPS reports in: OL, no hold */
    CHECK("second glitch after a fresh status is held too",
          status_hold(0, OL, &hold, 300 * SEC) == OL);
    CHECK("second glitch starts its own clock, not the first one's",
          hold == 300 * SEC);

    return fails ? 1 : 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        open(c, "w").write(TESTS)
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", INC, "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            print("  FAIL status_hold.h does not compile on the host:")
            print(r.stderr.strip()[:1500])
            return 1
        rc = subprocess.run([exe]).returncode
    print(f"\n{'PASS' if rc == 0 else 'FAIL'}: status_hold")
    return rc


if __name__ == "__main__":
    sys.exit(main())
