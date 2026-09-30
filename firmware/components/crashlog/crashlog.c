/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crashlog.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "nvs.h"
#include "xtensa_context.h"

static const char *TAG = "crashlog";

#define MAGIC       0x43524153u     /* "CRAS" */
#define DEPTH       8
#define NVS_NS      "upsa"
#define NVS_KEY     "lastcrash"

/* RTC_NOINIT: untouched by a software reset, garbage after power-on, hence
 * the magic. Written only by the panic handler below. */
typedef struct {
    uint32_t magic;
    uint8_t  core;
    uint8_t  depth;
    char     reason[56];
    uint32_t pc[DEPTH];
} crash_rec_t;
static RTC_NOINIT_ATTR crash_rec_t s_rec;

static char s_last[200];

void __real_esp_panic_handler(panic_info_t *info);

/* Runs inside the panic handler: from IRAM, possibly with the flash cache
 * off, on a stack that may be the one that just broke. So no library calls,
 * no allocation, no logging -- copy what is there and hand over. */
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    s_rec.magic = 0;
    s_rec.core = (uint8_t)info->core;
    const char *r = info->reason ? info->reason : "?";
    size_t i = 0;
    for (; r[i] && i < sizeof(s_rec.reason) - 1; i++) s_rec.reason[i] = r[i];
    s_rec.reason[i] = '\0';

    s_rec.depth = 0;
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    if (f) {
        esp_backtrace_frame_t fr = {
            .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = (void *)f,
        };
        s_rec.pc[s_rec.depth++] = esp_cpu_process_stack_pc(fr.pc);
        while (s_rec.depth < DEPTH && fr.next_pc && esp_backtrace_get_next_frame(&fr)) {
            s_rec.pc[s_rec.depth++] = esp_cpu_process_stack_pc(fr.pc);
        }
    } else if (info->addr) {
        s_rec.pc[s_rec.depth++] = (uint32_t)info->addr;
    }
    s_rec.magic = MAGIC;
    __real_esp_panic_handler(info);
}

void crashlog_init(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool panicked = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                    rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
    nvs_handle_t h;
    if (panicked && s_rec.magic == MAGIC) {
        /* IDF's reasons for the watchdogs already name the CPU. */
        int n = strstr(s_rec.reason, "CPU")
              ? snprintf(s_last, sizeof(s_last), "%.55s, bt", s_rec.reason)
              : snprintf(s_last, sizeof(s_last), "%.55s on CPU%u, bt", s_rec.reason, s_rec.core);
        for (int i = 0; i < s_rec.depth && i < DEPTH && n < (int)sizeof(s_last) - 12; i++) {
            n += snprintf(s_last + n, sizeof(s_last) - n, " 0x%08lx",
                          (unsigned long)s_rec.pc[i]);
        }
        /* Which build the addresses belong to: the one running now, which is
         * the one that crashed unless a rollback happened in between. */
        snprintf(s_last + n, sizeof(s_last) - n, " (%s)", esp_app_get_description()->version);
        ESP_LOGE(TAG, "last boot crashed: %s", s_last);
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY, s_last);
            nvs_commit(h);
            nvs_close(h);
        }
    } else if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_last);
        if (nvs_get_str(h, NVS_KEY, s_last, &len) != ESP_OK) s_last[0] = '\0';
        nvs_close(h);
    }
    s_rec.magic = 0;
}

const char *crashlog_last(void)
{
    return s_last;
}

void crashlog_clear(void)
{
    s_last[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}
