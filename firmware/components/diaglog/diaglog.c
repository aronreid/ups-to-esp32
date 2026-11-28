/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diaglog.h"

#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "ota.h"

static const char *TAG = "diaglog";

/* Rev A (no PSRAM, 4 MB part, memory tight): 6 KB of internal RAM. A board
 * with PSRAM: 32 KB there. Features follow capabilities, not board names. */
#if CONFIG_UPSA_BOARD_PSRAM_CLASS
#define RING_BYTES  (32 * 1024)
#define RING_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define RING_BYTES  (6 * 1024)
#define RING_CAPS   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
#define LINE_MAX_B  256     /* a longer line is cut */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static dl_ring_t    s_ring;
static dl_limit_t   s_limit;
static vprintf_like_t s_prev;            /* what the hook forwards to */
static bool         s_hooked;
static volatile bool s_on;               /* capture running (gate open) */
static volatile uint8_t s_pending;       /* bit per reason, see s_reasons */
static volatile int32_t s_last_ok_s = -1;
static void       (*s_kick)(void);

static const char *const s_reasons[] = { "crash", "usb_fail", "asked", "daily" };
#define N_REASONS  (sizeof(s_reasons) / sizeof(s_reasons[0]))

/* The USB host's own tags, raised while the gate is open: what the board's
 * enumeration does is the first thing a "my UPS never shows up" report needs.
 * DEBUG only where the build compiles it in; the default build's maximum is
 * INFO, where this leaves them as they were. */
static const char *const s_usb_tags[] = { "ENUM", "HUB", "USBH", "USB HOST" };
#if CONFIG_LOG_MAXIMUM_LEVEL >= 4
#define USB_LEVEL  ESP_LOG_DEBUG
#else
#define USB_LEVEL  ESP_LOG_INFO
#endif

/* The hook runs in whatever task logs, some with little stack to spare, so
 * the line is formatted into one static buffer, not on the caller's stack.
 * A task that finds it in use (another is formatting at that instant) skips
 * the ring for that line; it still reaches the console below. */
static char          s_line[LINE_MAX_B];
static volatile bool s_line_busy;
static dl_filter_t   s_filter;       /* dl_filter_init() at enable; guarded by s_line_busy */    /* guarded by s_line_busy */

static int hook(const char *fmt, va_list ap)
{
    if (s_on && !__atomic_test_and_set(&s_line_busy, __ATOMIC_ACQUIRE)) {
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(s_line, sizeof(s_line), fmt, cp);
        va_end(cp);
        size_t len = n <= 0 ? 0 : (size_t)n < sizeof(s_line) ? (size_t)n : sizeof(s_line) - 1;
        if (len && (size_t)n >= sizeof(s_line)) s_line[len - 1] = '\n';   /* cut line still ends */
        if (len && dl_keep_line(&s_filter, s_line, len, esp_timer_get_time() / 1000000)) {
            dl_redact_line(s_line, len);
            portENTER_CRITICAL_SAFE(&s_mux);
            dl_ring_put(&s_ring, s_line, len);
            portEXIT_CRITICAL_SAFE(&s_mux);
        }
        __atomic_clear(&s_line_busy, __ATOMIC_RELEASE);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

static bool gate_now(void)
{
    ota_status_t o;
    ota_get(&o);
    return dl_gate(o.rc_opt);
}

void diaglog_poll(void)
{
    bool want = gate_now();
    if (want == s_on) return;
    if (want) {
        if (!s_ring.buf) {
            char *b = heap_caps_malloc(RING_BYTES, RING_CAPS);
            if (!b) { ESP_LOGW(TAG, "no memory for the log ring"); return; }
            portENTER_CRITICAL(&s_mux);
            dl_ring_init(&s_ring, b, RING_BYTES);
            dl_filter_init(&s_filter);
            dl_limit_init(&s_limit);
            portEXIT_CRITICAL(&s_mux);
        }
        if (!s_hooked) {
            s_prev = esp_log_set_vprintf(hook);
            s_hooked = true;
        }
        for (size_t i = 0; i < sizeof(s_usb_tags) / sizeof(s_usb_tags[0]); i++) {
            esp_log_level_set(s_usb_tags[i], USB_LEVEL);
        }
        s_on = true;
        ESP_LOGI(TAG, "diagnostic log on (%u bytes)", (unsigned)RING_BYTES);
    } else {
        /* Stop capturing and sending. The ring stays until restart, and the
         * hook only forwards from here on. */
        s_on = false;
        s_pending = 0;
        for (size_t i = 0; i < sizeof(s_usb_tags) / sizeof(s_usb_tags[0]); i++) {
            esp_log_level_set(s_usb_tags[i], CONFIG_LOG_DEFAULT_LEVEL);
        }
        ESP_LOGI(TAG, "diagnostic log off");
    }
}

bool diaglog_enabled(void) { return s_on; }

size_t diaglog_capacity(void) { return s_on ? s_ring.cap : 0; }

void diaglog_request_upload(const char *reason)
{
    if (!s_on || !reason) return;
    int bit = -1;
    for (size_t i = 0; i < N_REASONS; i++) if (!strcmp(reason, s_reasons[i])) bit = (int)i;
    if (bit < 0) return;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = dl_request_ok(&s_limit, reason, esp_timer_get_time() / 1000000);
    portEXIT_CRITICAL(&s_mux);
    if (!ok) return;
    s_pending |= (uint8_t)(1u << bit);
    if (s_kick) s_kick();
}

const char *diaglog_take_request(void)
{
    if (!s_on) { s_pending = 0; return NULL; }
    uint8_t p = s_pending;
    if (!p) return NULL;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = dl_send_ok(&s_limit, esp_timer_get_time() / 1000000);
    portEXIT_CRITICAL(&s_mux);
    if (!ok) return NULL;               /* over the day's limit: keep it pending */
    s_pending = 0;
    for (size_t i = 0; i < N_REASONS; i++) if (p & (1u << i)) return s_reasons[i];
    return NULL;
}

size_t diaglog_snapshot(char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = '\0';
    if (!s_ring.buf) return 0;
    /* Not under the spinlock: copying 16 KB out of PSRAM with interrupts
     * masked would stall the USB host. Every index is taken modulo the
     * ring's size, so a line written during the copy can at worst garble the
     * oldest edge of the text, which the scrub then treats like any other. */
    size_t n = dl_ring_read(&s_ring, out, cap);
    diaglog_scrub(out);
    return n;
}

void diaglog_note_upload(bool ok)
{
    int64_t now = esp_timer_get_time() / 1000000;
    portENTER_CRITICAL(&s_mux);
    dl_sent(&s_limit, now);
    portEXIT_CRITICAL(&s_mux);
    if (ok) s_last_ok_s = (int32_t)now;
}

int32_t diaglog_last_upload_ago_s(void)
{
    int32_t t = s_last_ok_s;
    return t < 0 ? -1 : (int32_t)(esp_timer_get_time() / 1000000) - t;
}

void diaglog_set_kick(void (*kick)(void)) { s_kick = kick; }
