/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A port of usbhid-ups.c's status handling (NUT, GPL-2.0-or-later). Logging,
 * calibration timing and the throttled OL+DISCHRG messages are NUT's
 * bookkeeping around it and are left out; the words produced are the same.
 */
#include "ups_nutstatus.h"
#include <stdio.h>
#include <string.h>

void ups_nutstatus_apply(uint32_t *bits, int token, bool clear)
{
    if (token < 0 || token >= NST_COUNT) return;
    /* "Only neuter the other if we know the opposite to be true." */
    if (!clear) {
        if (token == NST_ONLINE)          *bits &= ~NST(NST_OFFLINE);
        if (token == NST_OFFLINE)         *bits &= ~NST(NST_ONLINE);
        if (token == NST_FULLYCHARGED)    *bits &= ~NST(NST_NOTFULLYCHARGED);
        if (token == NST_NOTFULLYCHARGED) *bits &= ~NST(NST_FULLYCHARGED);
    }
    if (clear) *bits &= ~NST(token);
    else       *bits |=  NST(token);
}

/* status_set(): append a word unless it is there already. */
static void word(char *out, size_t cap, const char *w)
{
    size_t wl = strlen(w);
    for (const char *p = out; (p = strstr(p, w)) != NULL; p += wl) {
        bool start = (p == out || p[-1] == ' ');
        bool end   = (p[wl] == '\0' || p[wl] == ' ');
        if (start && end) return;
    }
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "%s%s", n ? " " : "", w);
}

void ups_nutstatus_string(uint32_t b, int charge, char *out, size_t cap)
{
    out[0] = '\0';
    if (b & NST(NST_CAL)) word(out, cap, "CAL");
    if (b & NST(NST_OFFLINE)) word(out, cap, "OB");
    if (!(b & NST(NST_ONLINE))) {
        if (b & NST(NST_OFFLINE))      word(out, cap, "OB");
        else if (b & NST(NST_DISCHRG)) word(out, cap, "OB");  /* no power state, discharging */
    } else if (b & NST(NST_DISCHRG)) {
        word(out, cap, "OL");          /* onlinedischarge_* off: NUT's default */
    } else {
        word(out, cap, "OL");
    }
    if ((b & NST(NST_DISCHRG)) && !(b & NST(NST_DEPLETED))) word(out, cap, "DISCHRG");
    if (b & NST(NST_CHRG)) {
        if (b & NST(NST_NOTFULLYCHARGED)) {
            word(out, cap, "CHRG");
        } else if (!(b & NST(NST_FULLYCHARGED)) && charge > 0 && charge < 100) {
            word(out, cap, "CHRG");
        }
    }
    if (b & (NST(NST_LOWBATT) | NST(NST_TIMELIMITEXP) | NST(NST_SHUTDOWNIMM))) word(out, cap, "LB");
    if (b & NST(NST_OVERLOAD)) word(out, cap, "OVER");
    if (b & (NST(NST_REPLACEBATT) | NST(NST_NOBATTERY))) word(out, cap, "RB");
    if (b & NST(NST_TRIM))  word(out, cap, "TRIM");
    if (b & NST(NST_BOOST)) word(out, cap, "BOOST");
    if (b & (NST(NST_BYPASSAUTO) | NST(NST_BYPASSMAN))) word(out, cap, "BYPASS");
    if (b & NST(NST_OFF)) word(out, cap, "OFF");
}

void ups_nutstatus_alarm(uint32_t b, char *out, size_t cap)
{
    static const struct { int t; const char *msg; } A[] = {
        { NST_REPLACEBATT,   "Replace battery!" },
        { NST_SHUTDOWNIMM,   "Shutdown imminent!" },
        { NST_FANFAIL,       "Fan failure!" },
        { NST_NOBATTERY,     "No battery installed!" },
        { NST_BATTVOLTLO,    "Battery voltage too low!" },
        { NST_BATTVOLTHI,    "Battery voltage too high!" },
        { NST_CHARGERFAIL,   "Battery charger fail!" },
        { NST_OVERHEAT,      "Temperature too high!" },
        { NST_COMMFAULT,     "Internal UPS fault!" },
        { NST_AWAITINGPOWER, "Awaiting power!" },
        { NST_BYPASSAUTO,    "Automatic bypass mode!" },
        { NST_BYPASSMAN,     "Manual bypass mode!" },
    };
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(A) / sizeof(A[0]); i++) {
        if (!(b & NST(A[i].t))) continue;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? " " : "", A[i].msg);
    }
}

const char *ups_nutstatus_transfer_reason(uint32_t b)
{
    if (b & NST(NST_VRANGE)) return "input voltage out of range";
    if (b & NST(NST_FRANGE)) return "input frequency out of range";
    return NULL;
}
