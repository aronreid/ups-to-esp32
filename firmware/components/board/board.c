/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "board";

static board_status_t s_status = BOARD_STATUS_BOOTING;

#if BOARD_HAS_VBUS_SWITCH || BOARD_HAS_VBUS_FAULT
/* What firmware last asked of the load switch. Only meaningful where one
 * exists; see board_vbus_fault() for why it is tracked. Guarded because on a
 * board with neither line both users compile out, and an unused static is a
 * -Werror failure in the host compile that test_board_targets.py runs. */
static bool s_vbus_on;
#endif

#if BOARD_HAS_STATUS_LEDS
/* 100ms tick. Patterns are expressed as 8-slot bitmaps, one bit per tick, so a
 * blink costs a shift rather than a timer per LED. */
#define LED_TICK_MS 100

static void led_task(void *arg)
{
    uint8_t phase = 0;
    for (;;) {
        uint8_t green = 0, red = 0;
        switch (s_status) {
        case BOARD_STATUS_BOOTING:      green = 0xFF; red = 0x00; break;
        case BOARD_STATUS_PROVISIONING: green = 0xAA; red = 0x00; break; /* fast */
        case BOARD_STATUS_CONNECTING:   green = 0xF0; red = 0x00; break; /* slow */
        case BOARD_STATUS_ONLINE:       green = 0xFF; red = 0x00; break;
        case BOARD_STATUS_NO_UPS:       green = 0xFF; red = 0xF0; break;
        case BOARD_STATUS_FAULT:        green = 0x00; red = 0xFF; break;
        }
        gpio_set_level(BOARD_PIN_LED_STATUS, (green >> phase) & 1);
        gpio_set_level(BOARD_PIN_LED_FAULT,  (red   >> phase) & 1);
        phase = (uint8_t)((phase + 1) & 7);
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}
#endif

static const char *status_name(board_status_t s)
{
    switch (s) {
    case BOARD_STATUS_BOOTING:      return "booting";
    case BOARD_STATUS_PROVISIONING: return "provisioning AP";
    case BOARD_STATUS_CONNECTING:   return "connecting";
    case BOARD_STATUS_ONLINE:       return "online";
    case BOARD_STATUS_NO_UPS:       return "online, no UPS";
    case BOARD_STATUS_FAULT:        return "fault";
    default:                        return "?";
    }
}

void board_status_set(board_status_t state)
{
    /* The supervisor calls this twice a second; only a change is news. */
    if (state == s_status) return;
    s_status = state;
    ESP_LOGI(TAG, "status: %s", status_name(state));
}

board_status_t board_status_get(void)
{
    return s_status;
}

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "target: %s", BOARD_NAME);

#if BOARD_HAS_VBUS_SWITCH
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << BOARD_PIN_VBUS_EN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&en));
    /* Leave VBUS off until ups_hid is ready to enumerate, so a UPS that is
     * already attached at boot gets a clean cold start rather than being
     * half-enumerated by a driver that is not listening yet. */
    gpio_set_level(BOARD_PIN_VBUS_EN, 0);
#endif

#if BOARD_HAS_VBUS_FAULT
    gpio_config_t flag = {
        .pin_bit_mask = 1ULL << BOARD_PIN_VBUS_FAULT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* open drain, external 10k too */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&flag));
#endif

#if BOARD_HAS_STATUS_LEDS
    gpio_config_t leds = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LED_STATUS) |
                        (1ULL << BOARD_PIN_LED_FAULT),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&leds));
    gpio_set_level(BOARD_PIN_LED_STATUS, 0);
    gpio_set_level(BOARD_PIN_LED_FAULT, 0);
#endif

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_PIN_FACTORY_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

#if BOARD_HAS_STATUS_LEDS
    if (xTaskCreate(led_task, "led", 2048, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;   /* not fatal elsewhere, but says so honestly */
    }
#endif

    return ESP_OK;
}

esp_err_t board_vbus_set(bool on)
{
#if BOARD_HAS_VBUS_SWITCH
    s_vbus_on = on;
    gpio_set_level(BOARD_PIN_VBUS_EN, on ? 1 : 0);
    ESP_LOGI(TAG, "VBUS %s", on ? "on" : "off");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "VBUS %s requested, but this board hardwires VBUS", on ? "on" : "off");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t board_vbus_cycle(void)
{
#if BOARD_HAS_VBUS_SWITCH
    ESP_LOGW(TAG, "power-cycling UPS USB interface");
    board_vbus_set(false);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_UPSA_VBUS_CYCLE_MS));
    board_vbus_set(true);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "cannot power-cycle UPS: no load switch on this board. "
                  "Unplug and replug the UPS by hand to test the recovery path.");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool board_vbus_fault(void)
{
#if BOARD_HAS_VBUS_FAULT
    /* A fault reading only means anything while the switch is commanded ON.
     *
     * FLG is open drain with a 10k pull-up, so the pin reads low for a real
     * overcurrent or thermal event -- and firmware deliberately holds VBUS off
     * twice: from board_init() until ups_task raises it, and for two seconds
     * (CONFIG_UPSA_VBUS_CYCLE_MS) on every power-cycle recovery. Reporting
     * whatever the pin says during those windows would light the red FAULT LED
     * and set vbus_fault on the status page every time the board recovered a
     * wedged UPS, which reads as "the hardware broke" at the exact moment it
     * is repairing itself.
     *
     * A board without a FLG line compiles this out. */
    if (!s_vbus_on) return false;
    return gpio_get_level(BOARD_PIN_VBUS_FAULT) == 0;  /* active low */
#else
    return false;
#endif
}

bool board_factory_button_pressed(void)
{
    return gpio_get_level(BOARD_PIN_FACTORY_BTN) == 0;  /* active low */
}
