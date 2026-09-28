#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The status page decodes ota_state_t by NUMBER. Do the two agree?

firmware/components/webui/www/index.html carries its own copy of the enum:

    const OTA={IDLE:0,CHECKING:1,AVAILABLE:2,...};

Nothing connects it to ota.h. Insert a state anywhere but the end, or append to
one file only, and every later state shifts by one: the page then draws
"Downloading" for an installed image, or offers an Install button for a state
that means the opposite. It compiles, CI is green, and the mistake is only
visible to someone watching the page at the moment it matters -- which, for a
firmware update, is the moment you are least able to investigate.

Also checks that every string field the page reads out of the ota object is
actually written by get_status, because a missing one reads as `undefined` and
prints that word to the owner.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OTA_H = os.path.join(ROOT, "firmware/components/ota/include/ota.h")
WEBUI = os.path.join(ROOT, "firmware/components/webui/webui.c")
PAGE  = os.path.join(ROOT, "firmware/components/webui/www/index.html")


def c_enum(text, name):
    """Names of a C enum in declaration order, values assumed sequential."""
    body = re.search(r"typedef enum\s*\{(.*?)\}\s*" + name + r"\s*;", text, re.S)
    if not body:
        return None
    out = []
    for line in body.group(1).splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).split("/*")[0].strip().rstrip(",")
        if not line or line.startswith("*") or line.startswith("//"):
            continue
        m = re.match(r"([A-Z_0-9]+)\s*(?:=\s*(\d+))?$", line)
        if m:
            out.append((m.group(1), int(m.group(2)) if m.group(2) else None))
    return out


def main():
    fails = 0
    h = open(OTA_H).read()
    page = open(PAGE).read()

    states = c_enum(h, "ota_state_t")
    if not states:
        print("  FAIL could not find ota_state_t in ota.h")
        return 1

    js = re.search(r"const OTA\s*=\s*\{(.*?)\}\s*;", page, re.S)
    if not js:
        print("  FAIL could not find the OTA map in index.html")
        return 1
    jsmap = {k: int(v) for k, v in re.findall(r"([A-Z_0-9]+)\s*:\s*(\d+)", js.group(1))}

    # Expected numbering from ota.h: sequential unless a member assigns one.
    n = 0
    expected = {}
    for name, explicit in states:
        if explicit is not None:
            n = explicit
        expected[name.replace("OTA_", "")] = n
        n += 1

    if set(expected) != set(jsmap):
        for k in sorted(set(expected) - set(jsmap)):
            print(f"  FAIL {k} is in ota.h but not in the page's OTA map")
            fails += 1
        for k in sorted(set(jsmap) - set(expected)):
            print(f"  FAIL {k} is in the page's OTA map but not in ota.h")
            fails += 1
    for k in sorted(set(expected) & set(jsmap)):
        if expected[k] != jsmap[k]:
            print(f"  FAIL {k} is {expected[k]} in ota.h but {jsmap[k]} in the page")
            fails += 1
    if not fails:
        print(f"  ok   {len(expected)} ota_state_t values match the page's map")

    # Every ota.* field the page reads must be emitted by get_status.
    served = set(re.findall(r'\\"(\w+)\\":', open(WEBUI).read()))
    read = set(re.findall(r"\bo\.(\w+)\b", page))
    missing = sorted(read - served)
    if missing:
        for f in missing:
            print(f"  FAIL the page reads ota.{f}, which get_status never sends")
        fails += len(missing)
    else:
        print(f"  ok   all {len(read)} ota fields the page reads are sent")

    # The confirm handshake: ota_confirm() must report whether it succeeded, and
    # its caller must use that rather than latching. v0.03 shipped with the
    # caller setting confirmed = true after a single call five seconds into
    # boot, so the rollback stayed armed for the life of the image and a power
    # cut downgraded the device to the version it had just replaced.
    main_c = open(os.path.join(ROOT, "firmware/main/main.c")).read()
    if not re.search(r"bool\s+ota_confirm\(void\)", h):
        print("  FAIL ota_confirm() must return bool so a caller can retry")
        fails += 1
    elif not re.search(r"=\s*ota_confirm\(\)", main_c):
        print("  FAIL main.c calls ota_confirm() and discards the result;"
              " the rollback then stays armed forever")
        fails += 1
    elif re.search(r"ota_confirm\(\);\s*\n\s*confirmed\s*=\s*true", main_c):
        print("  FAIL main.c latches 'confirmed' regardless of ota_confirm()")
        fails += 1
    else:
        print("  ok   ota_confirm() reports success and main.c retries until it does")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
