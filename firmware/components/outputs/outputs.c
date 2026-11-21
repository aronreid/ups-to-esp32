/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Outputs on the board: settings in NVS, the pins, Button presses timed by
 * esp_timer, and the press log. What may be saved and what a pin does after a
 * restart are in outputs_core.h.
 */
#include "outputs.h"
#include "board.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "outputs";

#define NVS_NS      "upsa"        /* netmgr_factory_reset() erases it: the outputs go too */
#define NVS_CFG     "out_cfg"
#define NVS_ON      "out_on"
#define CFG_VER     1

#if BOARD_HAS_OUTPUTS
static const gpio_num_t PINS[OUT_N] = BOARD_OUT_PINS;
#endif

static SemaphoreHandle_t s_lock;
static out_cfg_t         s_cfg;
static bool              s_on[OUT_N];
static int64_t           s_last_us[OUT_N];
static esp_timer_handle_t s_pulse[OUT_N];
static uint32_t          s_gen;
static void            (*s_listener)(void);
static uint32_t        s_cfg_gen;      /* bumped by every settings change */

static uint8_t           s_boot_levels;

static out_log_t s_log[OUT_LOG_N];
static int       s_log_head, s_log_n;

typedef struct {
    uint8_t   ver;
    out_cfg_t cfg;
} saved_cfg_t;

bool outputs_available(void) { return BOARD_HAS_OUTPUTS; }

int outputs_header_pin(int i)
{
#if BOARD_HAS_OUTPUTS
    return BOARD_OUT_HEADER_PIN0 + i;
#else
    (void)i;
    return 0;
#endif
}

int outputs_gpio(int i)
{
#if BOARD_HAS_OUTPUTS
    return (i >= 0 && i < OUT_N) ? (int)PINS[i] : -1;
#else
    (void)i;
    return -1;
#endif
}

/* ---- pins ---------------------------------------------------------------- */

#if BOARD_HAS_OUTPUTS
/* Drive output i to the level for `on`, without a glitch: the output register
 * is set while the pad is still held, then the hold moves to the new level. */
static void drive(int i, bool on)
{
    gpio_num_t g = PINS[i];
    gpio_set_level(g, out_level(on, s_cfg.pin[i].active_low));
    gpio_hold_dis(g);
    gpio_hold_en(g);
}

/* Make output i what its setting says, at state `on`. */
static void setup_pin(int i, bool on)
{
    gpio_num_t g = PINS[i];
    if (s_cfg.pin[i].mode == OUT_MODE_OFF) {
        /* Not driven: floating, as at power-up. Nothing pulls it either way,
         * so whatever is wired to it decides. */
        gpio_hold_dis(g);
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << g, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        return;
    }
    /* Level first, then direction: if the pad is held from before a restart
     * it does not move until drive() lets go of the hold. */
    gpio_set_level(g, out_level(on, s_cfg.pin[i].active_low));
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << g, .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    drive(i, on);
}
#else
static void drive(int i, bool on) { (void)i; (void)on; }
static void setup_pin(int i, bool on) { (void)i; (void)on; }
#endif

/* ---- settings ------------------------------------------------------------ */

static void load(void)
{
    out_cfg_defaults(&s_cfg);
    memset(s_on, 0, sizeof(s_on));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    saved_cfg_t sv;
    size_t n = sizeof(sv);
    if (nvs_get_blob(h, NVS_CFG, &sv, &n) == ESP_OK && n == sizeof(sv) && sv.ver == CFG_VER) {
        for (int i = 0; i < OUT_N; i++) sv.cfg.pin[i].name[OUT_NAME_MAX - 1] = '\0';
        if (out_cfg_check(&sv.cfg, NULL, 0)) s_cfg = sv.cfg;
        else ESP_LOGW(TAG, "saved settings do not check out; outputs stay off");
    }
    uint8_t bits = 0;
    if (nvs_get_u8(h, NVS_ON, &bits) == ESP_OK) {
        for (int i = 0; i < OUT_N; i++) s_on[i] = (bits >> i) & 1;
    }
    nvs_close(h);
}

static esp_err_t save_cfg(const out_cfg_t *c)
{
    saved_cfg_t sv = { .ver = CFG_VER, .cfg = *c };
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_CFG, &sv, sizeof(sv));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* The Switch states, for a restart. Only written when one changes. */
static void save_on(void)
{
    uint8_t bits = 0;
    for (int i = 0; i < OUT_N; i++) {
        /* Only what a restart brings back (out_restore()): a Switch that
         * does not come back costs no flash write when it changes. */
        if (s_cfg.pin[i].mode == OUT_MODE_SWITCH && s_cfg.pin[i].restore && s_on[i]) bits |= 1u << i;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t old = 0xff;
    if (nvs_get_u8(h, NVS_ON, &old) != ESP_OK || old != bits) {
        if (nvs_set_u8(h, NVS_ON, bits) == ESP_OK) nvs_commit(h);
    }
    nvs_close(h);
}

/* ---- log ----------------------------------------------------------------- */

static void log_add(int i, uint8_t act, out_src_t src, bool ok)
{
    time_t now = time(NULL);
    out_log_t *e = &s_log[s_log_head];
    e->up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    e->epoch = now > 1700000000 ? (int64_t)now : 0;
    e->idx = (uint8_t)i;
    e->act = act;
    e->src = (uint8_t)src;
    e->ok = ok;
    s_log_head = (s_log_head + 1) % OUT_LOG_N;
    if (s_log_n < OUT_LOG_N) s_log_n++;
}

int outputs_log(out_log_t *out, int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_log_n < max ? s_log_n : max;
    for (int k = 0; k < n; k++) {
        out[k] = s_log[(s_log_head - 1 - k + OUT_LOG_N) % OUT_LOG_N];
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ---- changes ------------------------------------------------------------- */

static void changed(void)
{
    s_gen++;
    void (*fn)(void) = s_listener;
    if (fn) fn();
}

static void pulse_end(void *arg)
{
    int i = (int)(intptr_t)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.pin[i].mode == OUT_MODE_BUTTON && s_on[i]) {
        s_on[i] = false;
        drive(i, false);
    }
    xSemaphoreGive(s_lock);
    changed();
}

/* A planned restart (an update, the page's Restart, the watchdog's own
 * esp_restart) lets go of a Button mid-press first: the pad hold would
 * otherwise keep it pressed through the restart, a second or two longer than
 * set. A Switch keeps its level. No lock: the scheduler may be gone, and a
 * pin write is atomic. A crash does not get here; its Button stays held
 * through the restart and is released by outputs_init(). */
static void release_buttons(void)
{
    for (int i = 0; i < OUT_N; i++) {
        if (s_cfg.pin[i].mode == OUT_MODE_BUTTON && s_on[i]) drive(i, false);
    }
}

esp_err_t outputs_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load();
#if BOARD_HAS_OUTPUTS
    /* What the pads are at before anything here drives them. An Off pin gets
     * a pull-down for the reading only, so a floating pin reads 0 rather than
     * noise; a driven or held pin is not moved by it. */
    for (int i = 0; i < OUT_N; i++) {
        gpio_input_enable(PINS[i]);
        if (s_cfg.pin[i].mode == OUT_MODE_OFF) gpio_pulldown_en(PINS[i]);
    }
    esp_rom_delay_us(200);
    for (int i = 0; i < OUT_N; i++) {
        if (gpio_get_level(PINS[i])) s_boot_levels |= 1u << i;
        gpio_pulldown_dis(PINS[i]);
    }
#endif
    for (int i = 0; i < OUT_N; i++) {
        bool on = out_boot_on(&s_cfg.pin[i], s_on[i]);
        s_on[i] = on;
        if (BOARD_HAS_OUTPUTS) setup_pin(i, on);
        if (on) log_add(i, 1, OUT_SRC_BOOT, true);
        const esp_timer_create_args_t a = {
            .callback = pulse_end, .arg = (void *)(intptr_t)i, .name = "out_pulse",
        };
        esp_timer_create(&a, &s_pulse[i]);
    }
    esp_register_shutdown_handler(release_buttons);
    int used = 0;
    for (int i = 0; i < OUT_N; i++) used += s_cfg.pin[i].mode != OUT_MODE_OFF;
    if (used) ESP_LOGI(TAG, "%d output(s) set up", used);
    return ESP_OK;
}

void outputs_get_cfg(out_cfg_t *out)
{
    if (!s_lock) { out_cfg_defaults(out); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t outputs_set_cfg(const out_cfg_t *c, char *why, size_t why_len)
{
    if (!BOARD_HAS_OUTPUTS) {
        if (why) snprintf(why, why_len, "this board has no outputs");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!out_cfg_check(c, why, why_len)) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = save_cfg(c);
    if (err == ESP_OK) {
        out_cfg_t old = s_cfg;
        s_cfg = *c;
        for (int i = 0; i < OUT_N; i++) {
            const out_pin_cfg_t *o = &old.pin[i], *n = &c->pin[i];
            if (o->mode != n->mode || o->active_low != n->active_low) {
                esp_timer_stop(s_pulse[i]);
                s_on[i] = false;
                setup_pin(i, false);
            }
        }
        save_on();
    } else if (why) {
        snprintf(why, why_len, "could not save the settings");
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        __atomic_fetch_add(&s_cfg_gen, 1, __ATOMIC_SEQ_CST);
        changed();
    }
    return err;
}

/* Under s_lock: may output i be changed by src now? Logs a refusal. */
static esp_err_t may_change(int i, uint8_t mode, out_src_t src, uint8_t act,
                            char *why, size_t why_len)
{
    if (s_cfg.pin[i].mode != mode) {
        if (why) snprintf(why, why_len, "output %d is not a %s", i + 1, out_mode_name(mode));
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_src_allowed(&s_cfg, src)) {
        if (why) snprintf(why, why_len, "outputs may not be operated over MQTT on this board");
        log_add(i, act, src, false);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!out_rate_ok(s_last_us[i], esp_timer_get_time())) {
        if (why) snprintf(why, why_len, "output %d changed less than half a second ago", i + 1);
        log_add(i, act, src, false);
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t outputs_set(int i, bool on, out_src_t src, char *why, size_t why_len)
{
    if (i < 0 || i >= OUT_N || !BOARD_HAS_OUTPUTS || !s_lock) {
        if (why) snprintf(why, why_len, "no such output");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.pin[i].mode == OUT_MODE_SWITCH && s_on[i] == on) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = may_change(i, OUT_MODE_SWITCH, src, on ? 1 : 0, why, why_len);
    if (err == ESP_OK) {
        s_on[i] = on;
        s_last_us[i] = esp_timer_get_time();
        drive(i, on);
        log_add(i, on ? 1 : 0, src, true);
        save_on();
        ESP_LOGI(TAG, "output %d (%s) %s from the %s", i + 1, s_cfg.pin[i].name,
                 on ? "on" : "off", out_src_name(src));
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) changed();
    return err;
}

esp_err_t outputs_press(int i, out_src_t src, char *why, size_t why_len)
{
    if (i < 0 || i >= OUT_N || !BOARD_HAS_OUTPUTS || !s_lock) {
        if (why) snprintf(why, why_len, "no such output");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = may_change(i, OUT_MODE_BUTTON, src, 2, why, why_len);
    if (err == ESP_OK && s_on[i]) {
        if (why) snprintf(why, why_len, "output %d is still being pressed", i + 1);
        log_add(i, 2, src, false);
        err = ESP_ERR_NOT_ALLOWED;
    }
    if (err == ESP_OK) {
        s_on[i] = true;
        s_last_us[i] = esp_timer_get_time();
        drive(i, true);
        esp_timer_start_once(s_pulse[i], (uint64_t)s_cfg.pin[i].pulse_ms * 1000);
        log_add(i, 2, src, true);
        ESP_LOGI(TAG, "output %d (%s) pressed for %u ms from the %s", i + 1, s_cfg.pin[i].name,
                 s_cfg.pin[i].pulse_ms, out_src_name(src));
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) changed();
    return err;
}

bool outputs_is_on(int i)
{
    if (i < 0 || i >= OUT_N || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on[i];
    xSemaphoreGive(s_lock);
    return on;
}

uint8_t outputs_boot_levels(void) { return s_boot_levels; }

uint8_t outputs_levels(void)
{
    uint8_t v = 0;
#if BOARD_HAS_OUTPUTS
    for (int i = 0; i < OUT_N; i++) {
        if (gpio_get_level(PINS[i])) v |= 1u << i;
    }
#endif
    return v;
}

uint32_t outputs_gen(void) { return s_gen; }

void outputs_set_listener(void (*fn)(void)) { s_listener = fn; }

uint32_t outputs_cfg_gen(void) { return __atomic_load_n(&s_cfg_gen, __ATOMIC_SEQ_CST); }
