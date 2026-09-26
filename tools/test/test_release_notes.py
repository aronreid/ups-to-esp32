#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does the update card get the right bullets out of a release body?

When an update is offered, the status page shows an abridged "what changed"
list -- the bullets under "## What changed" that release.yml writes above the
tag's notes -- and links to the release for the rest. The abridging runs on
the board, in firmware/components/ota/ota_notes.c, against whatever text a
person typed into a tag message. So it is compiled here, on the host, and run
against the shape release.yml really produces plus the ways a hand-written tag
goes wrong: wrapped bullets, too many, too long, none at all, non-ASCII, and an
output buffer too small to hold them.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OTA = os.path.join(ROOT, "firmware", "components", "ota")

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "ota_notes.h"
int main(int argc, char **argv) {
    size_t cap = argc > 1 ? (size_t)atoi(argv[1]) : 2048;
    static char body[65536]; size_t n = fread(body, 1, sizeof(body) - 1, stdin);
    body[n] = 0;
    char *out = malloc(cap);
    ota_abridge_notes(body, out, cap);
    fputs(out, stdout);
    free(out);
    return 0;
}
'''

# What release.yml writes: its heading, then the annotated tag's body verbatim.
RELEASE_BODY = """## What changed

Setup that tells you where the board went, and a recovery fix found on the
first Rev A boards.

 - Wi-Fi setup joins your network while the setup page is still open, and shows
   the address the board was given before it goes away.
 - Each board has its own name, ups-esp32-XXXX.local.
 - Power-cycling the UPS link no longer leaves a self-powered UPS unread.

Tested on a Rev A board as v0.06-rc2.

---

**Updating a board already running:** open its web UI.
 - this bullet is in the instructions, not the notes
"""

CASES = [
    ("the body release.yml writes", RELEASE_BODY, 2048, [
        "Wi-Fi setup joins your network while the setup page is still open, and "
        "shows the address the board was given before it goes away.",
        "Each board has its own name, ups-esp32-XXXX.local.",
        "Power-cycling the UPS link no longer leaves a self-powered UPS unread.",
    ]),
    ("no What changed heading", "Firmware for ups-adaptor.\n\n- a bullet\n", 2048, []),
    ("heading with prose only", "## What changed\n\nJust a paragraph.\n", 2048, []),
    ("empty body", "", 2048, []),
    ("stops at the next heading", "## What changed\n- one\n## Other\n- two\n", 2048, ["one"]),
    ("asterisk bullets", "## What changed\n* one\n* two\n", 2048, ["one", "two"]),
    ("CRLF line endings", "## What changed\r\n- one\r\n- two\r\n\r\nafter\r\n", 2048, ["one", "two"]),
]


def build(td):
    c = os.path.join(td, "t.c")
    open(c, "w").write(HARNESS)
    exe = os.path.join(td, "t")
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", os.path.join(OTA, "include"), "-o", exe, c,
                        os.path.join(OTA, "ota_notes.c")],
                       capture_output=True, text=True)
    if r.returncode:
        print("  FAIL ota_notes.c does not compile on the host:")
        print(r.stderr.strip()[:900])
        return None
    return exe


def run(exe, body, cap):
    r = subprocess.run([exe, str(cap)], input=body.encode(), capture_output=True)
    return r.stdout


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        exe = build(td)
        if not exe:
            return 1

        for name, body, cap, want in CASES:
            got = run(exe, body, cap).decode()
            items = got.split("\n") if got else []
            if items == want:
                print(f"  ok   {name}")
            else:
                print(f"  FAIL {name}\n       want {want}\n       got  {items}")
                fails += 1

        # Six at most: the card is a summary, the link has the rest.
        many = "## What changed\n" + "".join(f"- item {i}\n" for i in range(10))
        items = run(exe, many, 2048).decode().split("\n")
        if items == [f"item {i}" for i in range(6)]:
            print("  ok   capped at six items")
        else:
            print(f"  FAIL capped at six items: got {items}"); fails += 1

        # A long item is cut at 160 CHARACTERS (not bytes: é is two), never
        # inside a UTF-8 glyph, and says so with an ellipsis.
        long_item = "é" * 300
        out = run(exe, "## What changed\n- " + long_item + "\n", 2048)
        try:
            text = out.decode("utf-8")
            if len(text) == 161 and text.endswith("…") and set(text[:-1]) == {"é"}:
                print(f"  ok   long item cut to {len(text)} characters, valid UTF-8")
            else:
                print(f"  FAIL long item: {len(text)} characters"); fails += 1
        except UnicodeDecodeError:
            print("  FAIL long item was cut inside a UTF-8 character"); fails += 1

        # A wrapped item that is cut drops its remaining lines, not just one.
        wrapped = "## What changed\n- " + "x" * 170 + "\n  more text\n- next\n"
        items = run(exe, wrapped, 2048).decode().split("\n")
        if len(items) == 2 and items[0] == "x" * 160 + "…" and items[1] == "next":
            print("  ok   a cut item drops its continuation lines")
        else:
            print(f"  FAIL cut item continuation: {items}"); fails += 1

        # A buffer smaller than the notes: truncated and terminated, no overrun.
        for cap in (1, 2, 16):
            out = run(exe, RELEASE_BODY, cap)
            if len(out) <= cap - 1:
                print(f"  ok   {cap}-byte buffer: {len(out)} bytes out, terminated")
            else:
                print(f"  FAIL {cap}-byte buffer: {len(out)} bytes out"); fails += 1

    print(f"\n{'FAIL' if fails else 'PASS'}: release notes abridging")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
