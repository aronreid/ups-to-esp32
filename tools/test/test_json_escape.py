#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does /api/status survive a string with a quote in it?

It did not. The OTA refusal message names the two image identities it declined
to swap -- built as "ups-adaptor", this board runs "ups-adaptor-freenove" --
and those quotes went into the JSON unescaped, so the whole document was
invalid and the page rendered nothing at all. The guard had worked perfectly;
the report of it broke the page that was supposed to show it.

Every string in that response comes from somewhere that can contain a quote or
a backslash: a UPS's model and manufacturer fields, an SSID someone chose, an
error message assembled here. So jesc() is extracted from webui.c, compiled on
the host, and fed the inputs that break naive escaping -- then the result is
parsed with a real JSON parser rather than eyeballed.
"""
import json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WEBUI = os.path.join(ROOT, "firmware/components/webui/webui.c")

# Inputs that break naive escaping, and one that must survive untouched.
CASES = [
    'built as "ups-adaptor", this board runs "ups-adaptor-freenove"',
    'CyberPower "EC850LCD"',
    'back\\slash',
    'quote-then-backslash "\\"',
    'newline\nand\ttab\r',
    'bell\x07and\x01control',
    'plain ASCII, nothing to do',
    '',
    'trailing backslash\\',
    '"' * 40,
]

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
%(jesc)s
int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        /* the driver sends one hex-encoded input per line */
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        char in[2048]; size_t ni = 0;
        for (size_t i = 0; i + 1 < L && ni + 1 < sizeof(in); i += 2) {
            unsigned v; sscanf(line + i, "%%2x", &v); in[ni++] = (char)v;
        }
        in[ni] = 0;
        char out[200];
        printf("\"%%s\"\n", jesc(in, out, sizeof(out)));
    }
    return 0;
}
'''


def main():
    src = open(WEBUI).read()
    m = re.search(r"static const char \*jesc\(.*?\n\}\n", src, re.S)
    if not m:
        print("  FAIL could not find jesc() in webui.c")
        return 1
    jesc = m.group(0)

    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        open(c, "w").write(HARNESS % {"jesc": jesc})
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-o", exe, c], capture_output=True, text=True)
        if r.returncode:
            print("  FAIL jesc() does not compile on the host:")
            print(r.stderr.strip()[:900])
            return 1

        stdin = "".join(s.encode().hex() + "\n" for s in CASES)
        r = subprocess.run([exe], input=stdin, capture_output=True, text=True)
        if r.returncode:
            print("  FAIL harness exited %d" % r.returncode)
            return 1

    lines = r.stdout.splitlines()
    if len(lines) != len(CASES):
        print("  FAIL got %d results for %d cases" % (len(lines), len(CASES)))
        return 1

    fails = 0
    for want, got in zip(CASES, lines):
        try:
            parsed = json.loads(got)
        except Exception as e:
            print("  FAIL %r -> %s   (%s)" % (want[:40], got[:60], e))
            fails += 1
            continue
        # Truncation is allowed (the buffer is finite); corruption is not.
        if not want.startswith(parsed) and parsed != want:
            print("  FAIL %r round-tripped as %r" % (want[:40], parsed[:40]))
            fails += 1

    if not fails:
        print("  ok   %d hostile strings all escape to parseable JSON" % len(CASES))
        print("  ok   including the refusal message that broke /api/status")

    # Every "%s" inside a JSON string literal in get_status must be escaped.
    body = re.search(r"static esp_err_t get_status\(.*?\n\}\n", src, re.S)
    if body:
        raw = [l.strip() for l in body.group(0).splitlines()
               if re.search(r'\\"[a-z_]+\\":\\"%s\\"', l)]
        unescaped = 0
        for call in re.finditer(r"snprintf\(buf \+ n, sizeof\(buf\) - n,(.*?)\);",
                                body.group(0), re.S):
            text = call.group(1)
            quoted = len(re.findall(r'\\":\\"%s\\"', text))
            escaped = len(re.findall(r"jesc\(", text))
            if quoted > escaped:
                print("  FAIL a string field is emitted without jesc():"
                      " %s" % " ".join(text.split())[:90])
                unescaped += 1
        fails += unescaped
        if not unescaped:
            print("  ok   every string field in get_status goes through jesc()")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
