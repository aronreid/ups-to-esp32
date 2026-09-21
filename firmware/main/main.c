/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ups-adaptor: bridges a USB-only UPS to Wi-Fi as a NUT server.
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

    /* Hold BOOT at power-on to clear Wi-Fi credentials without a serial cable.
     * Checked before netmgr so a wrong-SSID device is recoverable. */
    if (board_factory_button_pressed()) {
        ESP_LOGW(TAG, "factory button held at boot, clearing configuration");
        netmgr_factory_reset();   /* reboots */
    }

    /* No-ops unless CONFIG_UPSA_HAVE_OLED, and tolerant of an absent module. */
    ESP_ERROR_CHECK(display_init());
    display_line(0, "BOOTING");

    ESP_ERROR_CHECK(netmgr_start());
    ESP_ERROR_CHECK(webui_start());
    ESP_ERROR_CHECK(display_start());
    ESP_ERROR_CHECK(ups_hid_start());
    ESP_ERROR_CHECK(nut_server_start(NUT_DEFAULT_PORT));
    ESP_ERROR_CHECK(ota_start());

    ESP_LOGI(TAG, "%s up", BOARD_NAME);

    status_supervisor();   /* never returns */
}
