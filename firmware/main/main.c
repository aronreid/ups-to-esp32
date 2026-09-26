/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * UPS to ESP32 Module: bridges a USB-only UPS to Wi-Fi as a NUT server.
 *
 * Startup order matters:
 *   board   -> GPIO sane, VBUS held off
 *   netmgr  -> Wi-Fi or provisioning AP
 *   webui   -> setup reachable even if the UPS never enumerates
 *   display -> optional OLED, so bring-up messages have somewhere to go
 *   ups_hid -> raises VBUS and starts polling
 *   nut     -> last, so it never serves a snapshot that was never populated
 */
#include "board.h"
#include "netmgr.h"
#include "ups_hid.h"
#include "nut_server.h"
#include "webui.h"
#include "ota.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "main";

/* Single place that maps device state onto the status LEDs. Runs in app_main's
 * own task once everything is started, so there is no extra stack for it. */
static void status_supervisor(void)
{
    /* An image installed over the air boots on trial and the bootloader reverts
     * it unless it says otherwise. Confirming in app_main instead would confirm
     * an image that panics a second later, so it happens here, from the loop
     * that is running once everything else is up.
     *
     * KEEP ASKING UNTIL IT TAKES. ota_confirm() refuses an image that has not
     * yet earned confirmation -- five minutes of uptime and three served HTTP
     * requests -- and the first opportunity to ask is seconds after boot. This
     * used to ask once and latch, so the gate refused it once and was never
     * asked again: v0.03 ran for eleven minutes still on trial, one power cut
     * away from silently downgrading itself. */
    bool confirmed = false;

    for (;;) {
        if (!confirmed) {
            confirmed = ota_confirm();
        }
        ups_data_t d;
        ups_hid_get(&d);

        board_status_t st;
        if (board_vbus_fault()) {
            st = BOARD_STATUS_FAULT;
        } else {
            switch (netmgr_state()) {
            case NETMGR_AP_PROVISIONING: st = BOARD_STATUS_PROVISIONING; break;
            case NETMGR_STA_CONNECTING:
            case NETMGR_STA_RETRYING:    st = BOARD_STATUS_CONNECTING;   break;
            case NETMGR_STA_CONNECTED:
                st = d.attached ? BOARD_STATUS_ONLINE : BOARD_STATUS_NO_UPS;
                break;
            default:                     st = BOARD_STATUS_BOOTING;      break;
            }
        }
        board_status_set(st);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* Press BOOT five times within four seconds: clear Wi-Fi, restart into setup.
 *
 * NOT "HOLD BOOT AT POWER-ON", which is what this used to be and cannot work:
 * BOOT is GPIO0, the S3's download-mode strap, so holding it while power comes
 * up starts the ROM flasher and this firmware never runs to see it. A board
 * given the wrong network had no way back without a serial cable.
 *
 * AND NOT "HOLD FOR TEN SECONDS". On Rev A, GPIO0 is also driven by the
 * CH340C's auto-reset transistor: a serial terminal that raises DTR alone --
 * `screen` does -- holds GPIO0 low exactly as a held button does, and would
 * wipe the network of anyone watching the console. A serial line cannot tap
 * the pin five times, and an esptool reset restarts the chip, counter and all.
 *
 * Presses are counted on the debounced falling edge: a held-low line is one
 * press, whatever its length. */
#define FACTORY_PRESSES   5
#define FACTORY_WINDOW_MS 4000

static void factory_gesture_task(void *arg)
{
    bool stable = false, prev = false;
    int count = 0;
    TickType_t first = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        bool raw = board_factory_button_pressed();
        if (raw != prev) { prev = raw; continue; }   /* same reading twice */
        if (raw == stable) continue;
        stable = raw;
        if (!stable) continue;                        /* presses, not releases */

        TickType_t now = xTaskGetTickCount();
        if (count == 0 || now - first > pdMS_TO_TICKS(FACTORY_WINDOW_MS)) {
            count = 0;
            first = now;
        }
        if (++count >= 2) {
            ESP_LOGI(TAG, "BOOT pressed %d of %d for a Wi-Fi reset", count, FACTORY_PRESSES);
        }
        if (count >= FACTORY_PRESSES) {
            ESP_LOGW(TAG, "BOOT pressed %d times: clearing Wi-Fi and restarting into setup",
                     FACTORY_PRESSES);
            netmgr_factory_reset();                   /* restarts */
        }
    }
}

#if CONFIG_UPSA_MEM_DIAG
/* Heap and stacks, measured. Stack is reported as bytes NEVER used, so a task
 * sized with a small figure here is the one to look at, and a large one is
 * memory given back by shrinking its stack. */
static void mem_diag_task(void *arg)
{
    static TaskStatus_t st[24];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI("memdiag", "heap internal free %u, lowest %u, largest block %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        UBaseType_t n = uxTaskGetSystemState(st, sizeof(st) / sizeof(st[0]), NULL);
        for (UBaseType_t i = 0; i < n; i++) {
            ESP_LOGI("memdiag", "  stack unused %5u  %s",
                     (unsigned)st[i].usStackHighWaterMark, st[i].pcTaskName);
        }
    }
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init());
    board_status_set(BOARD_STATUS_BOOTING);

    /* No-ops unless CONFIG_UPSA_HAVE_OLED, and tolerant of an absent module. */
    ESP_ERROR_CHECK(display_init());
    display_line(0, "BOOTING");

    ESP_ERROR_CHECK(netmgr_start());
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(display_start());
    ESP_ERROR_CHECK(ups_hid_start());
    ESP_ERROR_CHECK(nut_server_start(NUT_DEFAULT_PORT));
    ESP_ERROR_CHECK(ota_start());

    xTaskCreate(factory_gesture_task, "factory_btn", 2560, NULL, 2, NULL);
#if CONFIG_UPSA_MEM_DIAG
    xTaskCreate(mem_diag_task, "memdiag", 3072, NULL, 1, NULL);
#endif

    ESP_LOGI(TAG, "%s up", BOARD_NAME);

    status_supervisor();   /* never returns */
}
