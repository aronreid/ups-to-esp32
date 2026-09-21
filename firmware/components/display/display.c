/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "display.h"
#include "sdkconfig.h"

#if !defined(CONFIG_UPSA_HAVE_OLED)

/* OLED not fitted. Every entry point becomes a no-op so callers stay free of
 * conditional compilation. */
esp_err_t display_init(void)  { return ESP_OK; }
esp_err_t display_start(void) { return ESP_OK; }
void display_line(int row, const char *text) { (void)row; (void)text; }

#else

#include "font5x7.h"
#include "board.h"
#include "ups_hid.h"
#include "netmgr.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

#define OLED_ADDR      0x3C   /* 0x3D on some modules; jumper-selected */
#define OLED_W         128
#define OLED_H         64
#define OLED_PAGES     (OLED_H / 8)
#define REFRESH_MS     1000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[OLED_W * OLED_PAGES];   /* 1KB, page-major */
static bool s_ready;

static esp_err_t oled_cmd(uint8_t c)
{
    uint8_t buf[2] = { 0x00, c };            /* 0x00 = command stream */
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t oled_flush(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    /* Horizontal addressing mode was set at init, so the whole framebuffer
     * goes out in one run with the pointer wrapping automatically. */
    ESP_ERROR_CHECK(oled_cmd(0x21)); ESP_ERROR_CHECK(oled_cmd(0)); ESP_ERROR_CHECK(oled_cmd(OLED_W - 1));
    ESP_ERROR_CHECK(oled_cmd(0x22)); ESP_ERROR_CHECK(oled_cmd(0)); ESP_ERROR_CHECK(oled_cmd(OLED_PAGES - 1));

    /* One 0x40 data prefix, then the payload. Chunked to keep the transfer
     * buffer modest rather than pushing 1KB in a single transaction. */
    for (int p = 0; p < OLED_PAGES; p++) {
        uint8_t chunk[1 + OLED_W];
        chunk[0] = 0x40;                     /* 0x40 = data stream */
        memcpy(&chunk[1], &s_fb[p * OLED_W], OLED_W);
        esp_err_t err = i2c_master_transmit(s_dev, chunk, sizeof(chunk), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void fb_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

static void fb_text(int row, int col_px, const char *s)
{
    if (row < 0 || row >= OLED_PAGES) return;
    uint8_t *page = &s_fb[row * OLED_W];
    for (int x = col_px; *s && x + FONT_WIDTH <= OLED_W; s++, x += FONT_ADVANCE) {
        const uint8_t *g = font5x7[font_index(*s)];
        for (int i = 0; i < FONT_WIDTH; i++) page[x + i] = g[i];
    }
}

void display_line(int row, const char *text)
{
    if (!s_ready) return;
    memset(&s_fb[row * OLED_W], 0, OLED_W);
    fb_text(row, 0, text);
    oled_flush();
}

static void display_task(void *arg)
{
    char line[24];

    for (;;) {
        ups_data_t d;
        ups_hid_get(&d);

        fb_clear();

        /* Line 0: identity. Line 1: the IP, which is the reason this screen
         * earns its place -- it is how you find the web UI. */
        fb_text(0, 0, "UPS-ADAPTOR");

        switch (netmgr_state()) {
        case NETMGR_AP_PROVISIONING:
            fb_text(1, 0, "SETUP AP");
            fb_text(2, 0, "192.168.4.1");
            break;
        case NETMGR_STA_CONNECTED:
            snprintf(line, sizeof(line), "IP %s", netmgr_ip());
            fb_text(1, 0, line);
            break;
        case NETMGR_STA_RETRYING:
            fb_text(1, 0, "WIFI RETRYING");
            break;
        default:
            fb_text(1, 0, "WIFI ...");
            break;
        }

        if (!d.attached) {
            fb_text(4, 0, "UPS NOT FOUND");
            if (board_vbus_fault()) fb_text(5, 0, "VBUS FAULT");
        } else {
            if (ups_valid(d.battery_charge)) {
                snprintf(line, sizeof(line), "BATT %d%%", (int)d.battery_charge);
                fb_text(4, 0, line);
            }
            if (ups_valid(d.battery_runtime)) {
                snprintf(line, sizeof(line), "RUN %d MIN",
                         (int)(d.battery_runtime / 60));
                fb_text(5, 0, line);
            }
            if (ups_valid(d.input_voltage)) {
                snprintf(line, sizeof(line), "IN %dV", (int)d.input_voltage);
                fb_text(6, 0, line);
            }
            if (ups_valid(d.ups_load)) {
                snprintf(line, sizeof(line), "LOAD %d%%", (int)d.ups_load);
                fb_text(7, 0, line);
            }
        }

        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

esp_err_t display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) return err;

    /* An absent or unresponsive module must not be fatal: this is an optional
     * debug aid, and a board in a closet has to keep serving NUT regardless. */
    if (i2c_master_probe(s_bus, OLED_ADDR, 200) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED at 0x%02x, continuing without display", OLED_ADDR);
        return ESP_OK;
    }

    static const uint8_t init_seq[] = {
        0xAE,       /* display off                     */
        0xD5, 0x80, /* clock divide                    */
        0xA8, 0x3F, /* multiplex ratio, 64 rows        */
        0xD3, 0x00, /* display offset                  */
        0x40,       /* start line 0                    */
        0x8D, 0x14, /* charge pump on                  */
        0x20, 0x00, /* horizontal addressing mode      */
        0xA1,       /* segment remap                   */
        0xC8,       /* COM scan direction remapped     */
        0xDA, 0x12, /* COM pins config, 128x64         */
        0x81, 0x7F, /* contrast                        */
        0xD9, 0xF1, /* precharge                       */
        0xDB, 0x40, /* VCOM deselect                   */
        0xA4,       /* resume from RAM                 */
        0xA6,       /* non-inverted                    */
        0xAF,       /* display on                      */
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        err = oled_cmd(init_seq[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED init failed at byte %u, continuing", (unsigned)i);
            return ESP_OK;
        }
    }

    s_ready = true;
    fb_clear();
    oled_flush();
    ESP_LOGI(TAG, "OLED ready on SDA %d SCL %d",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
    return ESP_OK;
}

esp_err_t display_start(void)
{
    if (!s_ready) return ESP_OK;   /* nothing to refresh */
    if (xTaskCreate(display_task, "display", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* CONFIG_UPSA_HAVE_OLED */
