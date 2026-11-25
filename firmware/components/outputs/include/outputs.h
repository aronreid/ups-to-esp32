/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Outputs: the four expansion-header pins (J5 pins 4 to 7) as switches and
 * buttons the owner names and uses for whatever they like. Operated from the
 * page (behind the login) and, if the owner allows it, over MQTT. Never by the
 * fleet: nothing in components/fleet can reach this.
 *
 * A restart never makes an output click. Each driven pin's pad is held
 * (gpio_hold_en) at its level, which survives a software restart (an update,
 * a crash, the watchdog), and outputs_init() sets the same level again before
 * letting go of the hold. A Button is always released before anything else
 * runs; a Switch comes back as it was only if the owner chose that. At
 * power-up the pins float until outputs_init(), a few hundred milliseconds:
 * the docs say to use a relay module that stays off with its input floating.
 */
#ifndef UPSA_OUTPUTS_H
#define UPSA_OUTPUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "outputs_core.h"

/* Put every configured pin at its start-up level. Call once, right after
 * board_init(), before anything that can take long. Never fatal. */
esp_err_t outputs_init(void);

/* False on a board whose pins are not known (dev boards). */
bool outputs_available(void);

/* The J5 pin and GPIO of output i (0-based), for the page. */
int outputs_header_pin(int i);
int outputs_gpio(int i);

void outputs_get_cfg(out_cfg_t *out);

/* Check, save and apply. An output whose mode or polarity changes is turned
 * off first. ESP_ERR_INVALID_ARG with a sentence in `why` on a refusal. */
esp_err_t outputs_set_cfg(const out_cfg_t *c, char *why, size_t why_len);

/* Turn a Switch on or off. ESP_ERR_INVALID_STATE (not a Switch), or
 * ESP_ERR_NOT_ALLOWED (the source may not, or too soon after the last
 * change), each with `why`. Setting the state it already has is ESP_OK and
 * changes nothing. */
esp_err_t outputs_set(int i, bool on, out_src_t src, char *why, size_t why_len);

/* Press a Button: on for its pulse, then off. Refused like outputs_set(), and
 * while a press is still running. */
esp_err_t outputs_press(int i, out_src_t src, char *why, size_t why_len);

/* Is output i on now (a Switch on, or a Button mid-press)? */
bool outputs_is_on(int i);

/* The four pins' levels now, and as found at start-up before anything was
 * driven (a pin held through the restart reads its held level), as a bitmask,
 * bit i = output i. For the Debug card and the bench: with a jumper from one
 * pin to another, the second shows what the first did through a restart. */
uint8_t outputs_levels(void);
uint8_t outputs_boot_levels(void);

/* Bumped by every change of state or settings, for MQTT to notice. */
uint32_t outputs_gen(void);

/* Bumped by every change of settings only (outputs_set_cfg()), before the
 * listener is called for it. */
uint32_t outputs_cfg_gen(void);

/* Called (from whichever task made the change, or the timer task) after
 * every change of state or settings. Must be quick and must not call back
 * into this component. One listener. */
void outputs_set_listener(void (*fn)(void));

/* The press log, newest first: what happened, when and from where. */
typedef struct {
    uint32_t up_s;          /* seconds since boot */
    int64_t  epoch;         /* Unix time, 0 if the clock was not set */
    uint8_t  idx;           /* output, 0-based */
    uint8_t  act;           /* 0 off, 1 on, 2 press */
    uint8_t  src;           /* out_src_t */
    uint8_t  ok;            /* 1 done, 0 refused */
} out_log_t;
#define OUT_LOG_N 50
int outputs_log(out_log_t *out, int max);

#endif /* UPSA_OUTPUTS_H */
