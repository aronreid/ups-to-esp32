#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Board-target lockstep: does every target still have a complete pin map, and
is an unselected target still rejected loudly?

A pin map rots silently. It only breaks when someone flashes a board, and by
then the board exists. This compiles the board-dependent code for each target
with cc -fsyntax-only against tools/test/stubs, so it needs no ESP-IDF and runs
in under a second.

It also checks the negative case, which is the one that actually decays: if the
#error guard is ever removed or a target renamed, an unselected build would
silently inherit whichever branch happened to fall through -- and flash a wrong
pin map onto real hardware.

Borrowed from the christmas-tree-sensor repo, where the same check found a board
revision whose pin map no longer compiled while that hardware was being packaged
for fab.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STUBS = os.path.join(HERE, "stubs")
BOARD = os.path.join(ROOT, "firmware", "components", "board")

# One board is made. The Freenove devkit target was removed on 2026-09-25; a
# Rev B joins this list when it exists.
TARGETS = ["CONFIG_UPSA_BOARD_REV_A"]
SOURCES = [os.path.join(BOARD, "board.c")]

CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
          "-fsyntax-only", "-I", STUBS, "-I", os.path.join(BOARD, "include")]


def main():
    fails = 0
    for target in TARGETS:
        name = target.replace("CONFIG_UPSA_BOARD_", "")
        for src in SOURCES:
            r = subprocess.run(["cc"] + CFLAGS + ["-D" + target + "=1", src],
                               capture_output=True, text=True)
            if r.returncode == 0:
                print("  ok   %-10s %s" % (name, os.path.basename(src)))
            else:
                print("  FAIL %-10s %s" % (name, os.path.basename(src)))
                print("".join("       " + l + "\n" for l in r.stderr.splitlines()[:8]))
                fails += 1

    # The negative case: no board selected must fail, and fail for the right reason.
    probe = os.path.join(tempfile.mkdtemp(), "probe.c")
    open(probe, "w").write('#include "board.h"\nint main(void){return BOARD_PIN_I2C_SDA;}\n')
    r = subprocess.run(["cc", "-std=c11", "-fsyntax-only", "-I", STUBS,
                        "-I", os.path.join(BOARD, "include"), probe],
                       capture_output=True, text=True)
    if r.returncode == 0:
        print("  FAIL board.h ACCEPTED a build with no target selected")
        fails += 1
    elif "No target board selected" in r.stderr:
        print("  ok   no target selected is rejected by the intended #error")
    else:
        print("  FAIL rejected, but not by the board guard -- something else broke:")
        print("".join("       " + l + "\n" for l in r.stderr.splitlines()[:6]))
        fails += 1

    # Both targets must define every macro the application relies on.
    required = ["BOARD_NAME", "BOARD_PIN_VBUS_EN", "BOARD_PIN_VBUS_FAULT",
                "BOARD_PIN_I2C_SDA", "BOARD_PIN_I2C_SCL", "BOARD_PIN_FACTORY_BTN",
                "BOARD_PIN_LED_STATUS", "BOARD_PIN_LED_FAULT",
                "BOARD_HAS_VBUS_SWITCH", "BOARD_HAS_VBUS_FAULT", "BOARD_HAS_STATUS_LEDS"]
    for target in TARGETS:
        name = target.replace("CONFIG_UPSA_BOARD_", "")
        body = "#include \"board.h\"\n"
        for m in required:
            body += "#ifndef %s\n#error \"missing %s\"\n#endif\n" % (m, m)
        body += "int main(void){return 0;}\n"
        p = os.path.join(tempfile.mkdtemp(), "macros.c")
        open(p, "w").write(body)
        r = subprocess.run(["cc", "-std=c11", "-fsyntax-only", "-I", STUBS,
                            "-I", os.path.join(BOARD, "include"),
                            "-D" + target + "=1", p], capture_output=True, text=True)
        if r.returncode == 0:
            print("  ok   %-10s defines all %d required BOARD_* macros" % (name, len(required)))
        else:
            missing = [l.split("missing ")[1].strip(' "') for l in r.stderr.splitlines()
                       if "missing " in l]
            print("  FAIL %-10s missing: %s" % (name, ", ".join(missing) or r.stderr[:120]))
            fails += 1

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
