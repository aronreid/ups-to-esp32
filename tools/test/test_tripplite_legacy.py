#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tripp Lite legacy protocol: packets built and replies decoded as NUT does.

firmware/components/ups_hid/tripplite_legacy.c is a port of NUT's
drivers/tripplite_usb.c decoding. No legacy Tripp Lite (09ae:0001) has been on
this project's bench, so this is the evidence it is right: compiled on the
host and run against the example replies NUT's own source documents from real
units -- SMARTPRO's "S100_Z0", "L290D_X", "T7D2581", "D7187", "V1062XX", the
OMNIVS "Sbb" status codes -- plus the checksum arithmetic, the echoed-reply
trap, the binary 3005 variant, and the charge estimate's curve.

In NUT's notation "_" is a NUL byte, and replies end in a carriage return.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
UPS = os.path.join(ROOT, "firmware", "components", "ups_hid")

TESTS = r'''
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tripplite_legacy.h"

static int fails = 0;
#define CHECK(name, cond) do { if (cond) printf("  ok   %s\n", name); \
    else { printf("  FAIL %s\n", name); fails++; } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 0.06)

/* A reply as NUT writes it: '_' is NUL, then CR, padded to 8 bytes. */
static void reply(const char *txt, uint8_t out[TL_PKT])
{
    memset(out, 0, TL_PKT);
    size_t n = strlen(txt);
    for (size_t i = 0; i < n && i < TL_PKT; i++) out[i] = txt[i] == '_' ? 0 : (uint8_t)txt[i];
    if (n < TL_PKT) out[n] = 13;
}

int main(void)
{
    uint8_t p[TL_PKT], r[TL_PKT];

    /* ---- packets ---- */
    tl_build_cmd((const uint8_t *)"S", 2, p);
    CHECK("S packet is ':' 'S' checksum CR",
          p[0] == ':' && p[1] == 'S' && p[2] == (uint8_t)(255 - 'S') && p[3] == 13 && p[4] == 0);
    tl_build_cmd((const uint8_t *)"\0", 2, p);
    CHECK("protocol query is ':' NUL 0xFF CR", p[0] == ':' && p[1] == 0 && p[2] == 0xFF && p[3] == 13);
    tl_build_cmd((const uint8_t *)"W\0", 3, p);
    CHECK("watchdog-off is ':' 'W' NUL checksum CR",
          p[1] == 'W' && p[2] == 0 && p[3] == (uint8_t)(255 - 'W') && p[4] == 13);

    /* ---- reply matching ---- */
    tl_build_cmd((const uint8_t *)"D", 2, p);
    reply("D7187", r);
    CHECK("a D reply is accepted for D", tl_reply_is_for(p, r));
    reply("L290D_X", r);
    CHECK("an echoed L reply is REJECTED for D", !tl_reply_is_for(p, r));

    /* ---- protocol ---- */
    uint8_t pr[TL_PKT] = { 0, 0x30, 0x03, 13 };
    CHECK("protocol 3003 is SMARTPRO", tl_decode_protocol(tl_protocol_from_reply(pr)) == TL_SMARTPRO);
    CHECK("protocol 1001 is OMNIVS", tl_decode_protocol(0x1001) == TL_OMNIVS);
    CHECK("protocol 3005 is binary SMART", tl_decode_protocol(0x3005) == TL_SMART_3005 && tl_is_binary(TL_SMART_3005));
    CHECK("an unknown protocol is unknown", tl_decode_protocol(0x9999) == TL_UNKNOWN);

    /* ---- SMARTPRO, from NUT's documented replies ---- */
    tl_config_t c; tl_reading_t rd;
    tl_config_init(&c, TL_SMARTPRO);
    reply("V1062XX", r); tl_decode_v(&c, r);
    CHECK("V1062XX: 110 V input range", c.input_nominal == 110 && c.input_scaled == 110);
    CHECK("V1062XX: 36 V battery (06 x 6)", c.battery_nominal == 36);
    CHECK("V1062XX: 2 switchable load banks", c.load_banks == 2);

    tl_reading_init(&rd);
    reply("S100_Z0", r); tl_decode_s(&c, r, &rd);
    CHECK("S100_Z0: on line, nothing else, not stale", rd.status == TL_ST_OL && !rd.stale);
    reply("D7187", r); tl_decode_d(&c, r, &rd);
    CHECK("D7187: input 113 x 110 / 120 = 103 V (integer, as NUT)", NEAR(rd.input_voltage, 103));
    CHECK("D7187: 13.5 V per 12 V block", NEAR(rd.battery_v12, 13.5));
    CHECK("D7187: 40.5 V on a 36 V bank", NEAR(rd.battery_voltage, 40.5));
    reply("T7D2581", r); tl_decode_t(&c, r, &rd);
    CHECK("T7D2581: 60.0 Hz", NEAR(rd.input_frequency, 60.0));
    CHECK("T7D2581: 125 x 0.3636 - 21 = 24.45 C", NEAR(rd.temperature, 24.45));
    reply("L290D_X", r); tl_decode_l(&c, r, &rd);
    CHECK("L290D_X: 41% load", NEAR(rd.load, 41));
    tl_estimate_charge(&rd);
    CHECK("13.5 V per block estimates 100%", NEAR(rd.battery_charge, 100));

    tl_reading_init(&rd);
    reply("S101_Z0", r); r[4] = 1; tl_decode_s(&c, r, &rd);
    CHECK("SMARTPRO: s[4] bit 0 is on battery", (rd.status & TL_ST_OB) && !(rd.status & TL_ST_OL));
    tl_reading_init(&rd);
    reply("S000_Z0", r); tl_decode_s(&c, r, &rd);
    CHECK("SMARTPRO: s[1] '0' is low battery", rd.status & TL_ST_LB);
    tl_reading_init(&rd);
    reply("S200_Z0", r); tl_decode_s(&c, r, &rd);
    CHECK("SMARTPRO: s[1] '2' is replace battery", rd.status & TL_ST_RB);
    tl_reading_init(&rd);
    reply("S130_Z0", r); tl_decode_s(&c, r, &rd);
    CHECK("SMARTPRO: s[2] '3' is overload", rd.status & TL_ST_OVER);
    tl_reading_init(&rd);
    reply("S1Q0_Z0", r); tl_decode_s(&c, r, &rd);
    CHECK("an unrecognised status code marks the poll stale", rd.stale);

    /* ---- OMNIVS (1001): "Sbb", bb = 10 on line, 11 on battery ---- */
    tl_config_init(&c, TL_OMNIVS);
    tl_reading_init(&rd);
    reply("S10_XXX", r); tl_decode_s(&c, r, &rd);
    CHECK("OMNIVS S10: on line", rd.status == TL_ST_OL && !rd.stale);
    tl_reading_init(&rd);
    reply("S11_XXX", r); tl_decode_s(&c, r, &rd);
    CHECK("OMNIVS S11: on battery", rd.status == TL_ST_OB);
    reply("B0E10DA", r); tl_decode_b(&c, r, &rd);
    CHECK("OMNIVS B: 0x0E10 / 3600 x 120 = 120 V in", NEAR(rd.input_voltage, 120));
    CHECK("OMNIVS B: 0xDA / 16 = 13.6 V battery", NEAR(rd.battery_voltage, 13.625));
    reply("L00F0XX", r); tl_decode_l(&c, r, &rd);
    CHECK("OMNIVS L: 0x00F0 / 240 x 120 = 120 V out, and no load", NEAR(rd.output_voltage, 120) && rd.load == TL_ABSENT);

    /* ---- binary SMART (3005) ---- */
    tl_config_init(&c, TL_SMART_3005);
    uint8_t v3005[TL_PKT] = { 'V', 2, 0x00, 0x0C, 1, 13 };
    tl_decode_v(&c, v3005);
    CHECK("3005 V: binary 12 V battery, 120 V range, 1 bank",
          c.battery_nominal == 12 && c.input_nominal == 120 && c.load_banks == 1);
    tl_reading_init(&rd);
    uint8_t s3005[TL_PKT] = { 'S', 1, 0, 0, 1, 0, 0, 13 };
    tl_decode_s(&c, s3005, &rd);
    CHECK("3005 S: binary digits decode, on battery", rd.status == TL_ST_OB && !rd.stale);
    uint8_t d3005[TL_PKT] = { 'D', 0x00, 0x76, 0x00, 0x88, 13 };
    tl_decode_d(&c, d3005, &rd);
    CHECK("3005 D: 118 V in, 13.6 V battery", NEAR(rd.input_voltage, 118) && NEAR(rd.battery_voltage, 13.6));
    uint8_t l3005[TL_PKT] = { 'L', 37, 0, 13 };
    tl_decode_l(&c, l3005, &rd);
    CHECK("3005 L: binary load byte, 37%", NEAR(rd.load, 37));

    /* ---- charge curve ---- */
    tl_reading_init(&rd); rd.battery_v12 = 11.0f; tl_estimate_charge(&rd);
    CHECK("11.0 V is the 10% floor", NEAR(rd.battery_charge, 10));
    tl_reading_init(&rd); rd.battery_v12 = 12.2f; tl_estimate_charge(&rd);
    CHECK("12.2 V is 100 x sqrt(1.2 / 2.4) = 70%", NEAR(rd.battery_charge, 70));
    tl_reading_init(&rd); tl_estimate_charge(&rd);
    CHECK("no battery voltage, no charge estimate", rd.battery_charge == TL_ABSENT);

    /* ---- what a poll sends ---- */
    char cmds[4];
    size_t n = tl_poll_commands(TL_OMNIVS, cmds);
    CHECK("OMNIVS polls S, B, L", n == 3 && !memcmp(cmds, "SBL", 3));
    n = tl_poll_commands(TL_SMARTPRO, cmds);
    CHECK("SMARTPRO polls S, D, T, L", n == 4 && !memcmp(cmds, "SDTL", 4));

    printf("\n%s: Tripp Lite legacy protocol\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        open(c, "w").write(TESTS)
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", os.path.join(UPS, "include"), "-o", exe, c,
                            os.path.join(UPS, "tripplite_legacy.c"), "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            print("  FAIL tripplite_legacy.c does not compile on the host:")
            print(r.stderr.strip()[:1500])
            return 1
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
