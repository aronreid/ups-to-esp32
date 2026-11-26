/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Outputs, the parts that do not need the chip: what a setting may be, what
 * level a pin is driven to, what an output does after a restart, and how
 * often it may be pressed. Header-only and free of ESP-IDF, so
 * tools/test/test_outputs.py compiles it on the host.
 *
 * An output is one of the four expansion-header pins, J5 pins 4 to 7. The
 * owner decides what it drives (a relay module, a buzzer, a light) and names
 * it. It is off until the owner sets it up, and is never driven by the fleet.
 */
#ifndef UPSA_OUTPUTS_CORE_H
#define UPSA_OUTPUTS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OUT_N            4
#define OUT_NAME_MAX     24          /* bytes, with the NUL */
#define OUT_PULSE_MIN_MS 100
#define OUT_PULSE_MAX_MS 10000
#define OUT_PULSE_DEF_MS 500
/* At most one change per output per this long, from any source. A script
 * or an automation stuck in a loop must not chatter a relay. */
#define OUT_MIN_GAP_US   (500LL * 1000)

typedef enum {
    OUT_MODE_OFF    = 0,   /* not driven: the pin floats, as at power-up      */
    OUT_MODE_SWITCH = 1,   /* stays on or off until changed                    */
    OUT_MODE_BUTTON = 2,   /* on for pulse_ms, then off: a momentary contact   */
} out_mode_t;

typedef enum {
    OUT_SRC_PAGE = 0,      /* the page or the HTTP API, behind the login       */
    OUT_SRC_MQTT = 1,      /* the MQTT broker, if the owner allowed it         */
    OUT_SRC_BOOT = 2,      /* a Switch restored at start-up                    */
    OUT_SRC_N
} out_src_t;

typedef struct {
    uint8_t  mode;                   /* out_mode_t */
    bool     active_low;             /* "on" drives the pin low */
    bool     restore;                /* a Switch comes back as it was after a restart */
    uint16_t pulse_ms;               /* a Button's press */
    char     name[OUT_NAME_MAX];
} out_pin_cfg_t;

typedef struct {
    out_pin_cfg_t pin[OUT_N];
    bool          mqtt_control;      /* MQTT may operate the outputs */
} out_cfg_t;

static inline void out_cfg_defaults(out_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < OUT_N; i++) {
        c->pin[i].pulse_ms = OUT_PULSE_DEF_MS;
        snprintf(c->pin[i].name, sizeof(c->pin[i].name), "Output %d", i + 1);
    }
}

/* A name goes into the page, JSON and Home Assistant's discovery: printable
 * ASCII, no characters that need escaping anywhere, not blank. */
static inline bool out_name_ok(const char *s)
{
    size_t n = strnlen(s, OUT_NAME_MAX);
    if (n == 0 || n >= OUT_NAME_MAX) return false;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e) return false;
        if (ch == '"' || ch == '\\' || ch == '<' || ch == '>' || ch == '&' || ch == '+' ||
            ch == '#' || ch == '/') return false;
        if (ch != ' ') any = true;
    }
    return any;
}

/* Would this be saved? `why` says what is wrong, for the page. */
static inline bool out_cfg_check(const out_cfg_t *c, char *why, size_t why_len)
{
    for (int i = 0; i < OUT_N; i++) {
        const out_pin_cfg_t *p = &c->pin[i];
        if (p->mode > OUT_MODE_BUTTON) {
            if (why) snprintf(why, why_len, "output %d: unknown mode", i + 1);
            return false;
        }
        if (p->pulse_ms < OUT_PULSE_MIN_MS || p->pulse_ms > OUT_PULSE_MAX_MS) {
            if (why) snprintf(why, why_len, "output %d: the press must be 0.1 to 10 seconds", i + 1);
            return false;
        }
        if (!out_name_ok(p->name)) {
            if (why) snprintf(why, why_len,
                              "output %d: the name must be 1 to 23 letters, digits or spaces "
                              "(no \" \\ < > & + # /)", i + 1);
            return false;
        }
    }
    return true;
}

/* The pin level for a logical state. */
static inline int out_level(bool on, bool active_low) { return (on != active_low) ? 1 : 0; }

/* What an output is at start-up. Only a Switch told to remember comes back
 * on; a Button never fires by itself, whatever it was doing at the restart. */
static inline bool out_boot_on(const out_pin_cfg_t *c, bool was_on)
{
    return c->mode == OUT_MODE_SWITCH && c->restore && was_on;
}

/* May an output change now? last_us is its last change (0: never). */
static inline bool out_rate_ok(int64_t last_us, int64_t now_us)
{
    return last_us == 0 || now_us - last_us >= OUT_MIN_GAP_US;
}

/* May this source operate outputs at all? The page always may (it has
 * already checked the login); MQTT only if the owner allowed it. */
static inline bool out_src_allowed(const out_cfg_t *c, out_src_t src)
{
    if (src == OUT_SRC_MQTT) return c->mqtt_control;
    return src < OUT_SRC_N;
}

static inline const char *out_mode_name(uint8_t m)
{
    return m == OUT_MODE_SWITCH ? "switch" : m == OUT_MODE_BUTTON ? "button" : "off";
}

static inline int out_mode_parse(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "off")) return OUT_MODE_OFF;
    if (!strcmp(s, "switch")) return OUT_MODE_SWITCH;
    if (!strcmp(s, "button")) return OUT_MODE_BUTTON;
    return -1;
}

static inline const char *out_src_name(uint8_t s)
{
    return s == OUT_SRC_PAGE ? "page" : s == OUT_SRC_MQTT ? "mqtt" : s == OUT_SRC_BOOT ? "restart" : "?";
}

#endif /* UPSA_OUTPUTS_CORE_H */
