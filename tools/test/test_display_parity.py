#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Display parity: does the OLED show every state the LEDs show?

On Rev A the display module plugs into J3 and sits directly over D3 and D4. With
it fitted the LEDs cannot be seen, even through the case's light pipes, so the
screen is the only local indicator. A state the LEDs signal and the screen does
not is invisible on exactly the boards that paid for a screen.

Both sides switch on board_status_t with no default, so the compiler catches a
missed state with -Wswitch -- but only in a build with CONFIG_UPSA_HAVE_OLED set,
and the shipped sdkconfig leaves it off. This checks the source instead, so it
runs in every harness pass without ESP-IDF:

  1. every board_status_t value has a case in led_task() and in status_text()
  2. neither switch has a default, which would silence -Wswitch
  3. every state that lights the red LED is flagged as an alarm on the screen,
     so a fault is shown in reverse rather than as one more line of text
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, "firmware", "components")
BOARD_H = os.path.join(FW, "board", "include", "board.h")
BOARD_C = os.path.join(FW, "board", "board.c")
DISPLAY_C = os.path.join(FW, "display", "display.c")


def body_of(src, signature):
    """Text of the function whose definition starts with `signature`, by brace
    matching from its opening brace."""
    i = src.find(signature)
    if i < 0:
        return None
    j = src.index("{", i)
    depth = 0
    for k in range(j, len(src)):
        depth += {"{": 1, "}": -1}.get(src[k], 0)
        if depth == 0:
            return src[j:k + 1]
    return None


def cases(body):
    """Map each `case BOARD_STATUS_X:` to the text up to the next case or the
    end of the switch."""
    out = {}
    parts = re.split(r"case\s+(BOARD_STATUS_\w+)\s*:", body)
    for name, text in zip(parts[1::2], parts[2::2]):
        out[name] = text
    return out


def main():
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print(("  ok   " if ok else "  FAIL ") + msg)
        fails += 0 if ok else 1

    h = open(BOARD_H).read()
    m = re.search(r"typedef enum\s*{(.*?)}\s*board_status_t;", h, re.S)
    if not m:
        print("  FAIL board_status_t not found in board.h")
        return 1
    states = re.findall(r"^\s*(BOARD_STATUS_\w+)", m.group(1), re.M)
    check(len(states) > 0, "board.h declares %d states" % len(states))

    led = body_of(open(BOARD_C).read(), "static void led_task(")
    oled = body_of(open(DISPLAY_C).read(), "static const char *status_text(")
    check(led is not None, "board.c has led_task()")
    check(oled is not None, "display.c has status_text()")
    if led is None or oled is None:
        return 1

    led_cases, oled_cases = cases(led), cases(oled)
    for s in states:
        check(s in led_cases, "LEDs handle %s" % s)
        check(s in oled_cases, "OLED handles %s" % s)

    check("default" not in led, "led_task() has no default (keeps -Wswitch live)")
    check("default" not in oled, "status_text() has no default (keeps -Wswitch live)")

    # Red LED lit in any phase means the screen must flag it. led_task sets
    # `red = 0x..;` per state; anything but 0x00 lights it at some point.
    for s, text in led_cases.items():
        r = re.search(r"red\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
        red_on = bool(r) and int(r.group(1), 0) != 0
        alarm = "*alarm = true" in oled_cases.get(s, "")
        if red_on:
            check(alarm, "%s lights the red LED and is shown as an alarm" % s)
        else:
            check(not alarm, "%s is not red on the board, and not an alarm on screen" % s)

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
