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
#include "nut_server.h"
#include "ota.h"
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
#define TICK_MS        100      /* button poll */
#define REFRESH_MS     1000     /* redraw */
#define PAGE_MS        5000     /* alternate the two data pages */

/* Burn-in. An OLED in a closet shows the same rows for months, and the ones
 * that never change wear in first. Two defences: dim after a quiet spell, and
 * walk everything sideways a pixel at a time so no column is lit forever. An
 * alarm, a change of state or a BOOT press brings full brightness back, and
 * nothing dims while an alarm is showing. */
#define DIM_AFTER_MS   (5 * 60 * 1000)
#define SHIFT_MS       (2 * 60 * 1000)
#define CONTRAST_FULL  0x7F
#define CONTRAST_DIM   0x01

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[OLED_W * OLED_PAGES];   /* 1KB, page-major */
static bool s_ready;
static int s_xshift;                        /* burn-in walk, 0..3 px */

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
    /* Errors are returned, never ESP_ERROR_CHECK'd: that aborts, and a loose
     * wire on an optional debug display must not reboot a NUT server. */
    static const uint8_t window[] = { 0x21, 0, OLED_W - 1, 0x22, 0, OLED_PAGES - 1 };
    for (size_t i = 0; i < sizeof(window); i++) {
        esp_err_t err = oled_cmd(window[i]);
        if (err != ESP_OK) return err;
    }

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
    for (int x = col_px + s_xshift; *s && x + FONT_WIDTH <= OLED_W; s++, x += FONT_ADVANCE) {
        const uint8_t *g = font5x7[font_index(*s)];
        for (int i = 0; i < FONT_WIDTH; i++) page[x + i] = g[i];
    }
}

/* Reverse a text row, for states the red LED would be showing. */
static void fb_invert_row(int row)
{
    if (row < 0 || row >= OLED_PAGES) return;
    uint8_t *page = &s_fb[row * OLED_W];
    for (int x = 0; x < OLED_W; x++) page[x] ^= 0xFF;
}

/* What the LEDs are saying, in words. Fitted, the module covers both LEDs, so
 * this is the only way that state is seen locally -- see board_status_t.
 *
 * NO DEFAULT. A state added to board_status_t without a line here must fail
 * the build (-Wswitch), not fall through to a blank row. *alarm is set for
 * every state that lights the red LED. */
static const char *status_text(board_status_t st, bool *alarm)
{
    *alarm = false;
    switch (st) {
    case BOARD_STATUS_BOOTING:      return "STATUS BOOTING";
    case BOARD_STATUS_PROVISIONING: return "STATUS SETUP";
    case BOARD_STATUS_CONNECTING:   return "STATUS CONNECTING";
    case BOARD_STATUS_ONLINE:       return "STATUS OK";
    case BOARD_STATUS_NO_UPS:       *alarm = true; return "STATUS NO UPS";
    case BOARD_STATUS_FAULT:        *alarm = true; return "STATUS VBUS FAULT";
    }
    return "STATUS ?";   /* unreachable for a valid enum value */
}

void display_line(int row, const char *text)
{
    if (!s_ready) return;
    memset(&s_fb[row * OLED_W], 0, OLED_W);
    fb_text(row, 0, text);
    oled_flush();
}

/* What the UPS itself is doing, which the LEDs never showed: on battery is
 * the whole reason this device exists, and the screen used to say only how
 * much charge was left. *alarm for anything that wants a person's attention. */
static const char *power_text(const ups_data_t *d, bool *alarm)
{
    *alarm = false;
    if (!d->attached) return "UPS NOT FOUND";   /* status line already alarms */
    uint32_t st = d->status;
    if (st & UPS_STATUS_ONBATT) {
        *alarm = true;
        return (st & UPS_STATUS_LOWBATT) ? "ON BATTERY  LOW" : "ON BATTERY";
    }
    if (st & UPS_STATUS_OVERLOAD)    { *alarm = true; return "OVERLOAD"; }
    if (st & UPS_STATUS_REPLACEBATT) { *alarm = true; return "REPLACE BATTERY"; }
    if (st & UPS_STATUS_LOWBATT)     { *alarm = true; return "LOW BATTERY"; }
    if (st & UPS_STATUS_ONLINE) {
        return (st & UPS_STATUS_CHARGING) ? "ON LINE  CHARGING" : "ON LINE";
    }
    return "UPS STATUS UNKNOWN";
}

/* NUT's own status words, for anyone matching the screen against upsc. */
static void nut_flags(uint32_t st, char *out, size_t n)
{
    static const struct { uint32_t bit; const char *word; } words[] = {
        { UPS_STATUS_ONLINE, "OL" },       { UPS_STATUS_ONBATT, "OB" },
        { UPS_STATUS_LOWBATT, "LB" },      { UPS_STATUS_CHARGING, "CHRG" },
        { UPS_STATUS_DISCHARGE, "DISCHRG" }, { UPS_STATUS_REPLACEBATT, "RB" },
        { UPS_STATUS_OVERLOAD, "OVER" },
    };
    size_t len = (size_t)snprintf(out, n, "FLAGS");
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]) && len < n; i++) {
        if (st & words[i].bit) {
            len += (size_t)snprintf(out + len, n - len, " %s", words[i].word);
        }
    }
}

/* Rows 0-2 are the same on both pages, so an alarm is never paged away:
 *   0  the UPS model, or UPS-ADAPTOR with none attached
 *   1  what the UPS is doing (on line / on battery / ...)
 *   2  what the LEDs are showing (design rule 5)
 * Returns true if either state line is an alarm. */
static bool draw_header(const ups_data_t *d)
{
    bool ups_alarm, dev_alarm;

    fb_text(0, 0, (d->attached && d->model[0]) ? d->model : "UPS-ADAPTOR");

    fb_text(1, 0, power_text(d, &ups_alarm));
    if (ups_alarm) fb_invert_row(1);

    fb_text(2, 0, status_text(board_status_get(), &dev_alarm));
    if (dev_alarm) fb_invert_row(2);

    return ups_alarm || dev_alarm;
}

/* Page 0: the numbers people look for, and how to reach the device. */
static void draw_page_main(const ups_data_t *d)
{
    char line[32];

    if (d->attached) {
        char a[12] = "", b[12] = "";
        if (ups_valid(d->battery_charge))
            snprintf(a, sizeof(a), "BATT %d%%", (int)d->battery_charge);
        if (ups_valid(d->battery_runtime))
            snprintf(b, sizeof(b), "RUN %dM", (int)(d->battery_runtime / 60));
        snprintf(line, sizeof(line), "%-10s%s", a, b);
        fb_text(4, 0, line);

        a[0] = b[0] = '\0';
        if (ups_valid(d->input_voltage))
            snprintf(a, sizeof(a), "IN %dV", (int)d->input_voltage);
        if (ups_valid(d->ups_load))
            snprintf(b, sizeof(b), "LOAD %d%%", (int)d->ups_load);
        snprintf(line, sizeof(line), "%-10s%s", a, b);
        fb_text(5, 0, line);
    }

    /* The IP is the reason this screen earns its place: it is how you find
     * the web UI without hunting through a DHCP table. */
    switch (netmgr_state()) {
    case NETMGR_AP_PROVISIONING: fb_text(6, 0, "SETUP AP 192.168.4.1"); break;
    case NETMGR_STA_CONNECTED:
        snprintf(line, sizeof(line), "IP %s", netmgr_ip());
        fb_text(6, 0, line);
        break;
    case NETMGR_STA_RETRYING:    fb_text(6, 0, "WIFI RETRYING"); break;
    default:                     fb_text(6, 0, "WIFI ..."); break;
    }

    /* Proof that something is actually watching: Home Assistant, a NAS. */
    int n = nut_server_clients();
    snprintf(line, sizeof(line), "NUT %d CLIENT%s", n, n == 1 ? "" : "S");
    fb_text(7, 0, line);
}

/* Page 1: detail for whoever is standing in front of it with a problem. */
static void draw_page_detail(const ups_data_t *d)
{
    char line[32];

    if (d->attached) {
        char a[12] = "", b[12] = "";
        if (ups_valid(d->battery_voltage))
            snprintf(a, sizeof(a), "BATT %.1fV", (double)d->battery_voltage);
        if (ups_valid(d->output_voltage))
            snprintf(b, sizeof(b), "OUT %dV", (int)d->output_voltage);
        snprintf(line, sizeof(line), "%-11s%s", a, b);
        fb_text(4, 0, line);

        nut_flags(d->status, line, sizeof(line));
        fb_text(5, 0, line);
    }

    if (netmgr_state() == NETMGR_STA_CONNECTED) {
        snprintf(line, sizeof(line), "WIFI %d DBM", netmgr_rssi());
        fb_text(6, 0, line);
    }

    ota_status_t o;
    ota_get(&o);
    if (o.state == OTA_AVAILABLE && o.latest[0]) {
        snprintf(line, sizeof(line), "UPDATE %.14s", o.latest);   /* 21 columns */
    } else {
        snprintf(line, sizeof(line), "FW %.18s", o.running);
    }
    fb_text(7, 0, line);
}

static void set_contrast(uint8_t level)
{
    if (oled_cmd(0x81) == ESP_OK) oled_cmd(level);
}

static void display_task(void *arg)
{
    static const int8_t walk[] = { 0, 1, 2, 3, 2, 1 };
    TickType_t now = xTaskGetTickCount();
    TickType_t last_draw = 0, last_page = now, last_wake = now, last_shift = now;
    int page = 0, step = 0;
    bool dim = false, button_was = false, force = true;
    uint32_t last_sig = UINT32_MAX;

    for (;;) {
        now = xTaskGetTickCount();

        /* A press wakes the screen and flips the page. Five in four seconds
         * also clears Wi-Fi (main.c); flipping pages on the way is harmless. */
        bool button = board_factory_button_pressed();
        if (button && !button_was) {
            if (!dim) page ^= 1;
            last_page = last_wake = now;
            force = true;
        }
        button_was = button;

        if (now - last_page >= pdMS_TO_TICKS(PAGE_MS)) {
            page ^= 1;
            last_page = now;
            force = true;
        }
        if (now - last_shift >= pdMS_TO_TICKS(SHIFT_MS)) {
            step = (step + 1) % (int)sizeof(walk);
            s_xshift = walk[step];
            last_shift = now;
            force = true;
        }

        if (force || now - last_draw >= pdMS_TO_TICKS(REFRESH_MS)) {
            ups_data_t d;
            ups_hid_get(&d);

            /* A change of state is news: wake for it. Readings drift all the
             * time and are not. */
            uint32_t sig = (uint32_t)board_status_get() | (d.attached ? 0x100u : 0)
                         | (d.status << 16);
            if (sig != last_sig) {
                last_sig = sig;
                last_wake = now;
            }

            fb_clear();
            bool alarm = draw_header(&d);
            if (page == 0) draw_page_main(&d); else draw_page_detail(&d);
            oled_flush();

            bool want_dim = !alarm && now - last_wake >= pdMS_TO_TICKS(DIM_AFTER_MS);
            if (want_dim != dim) {
                set_contrast(want_dim ? CONTRAST_DIM : CONTRAST_FULL);
                dim = want_dim;
            }
            last_draw = now;
            force = false;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* Send the init sequence. Returns the index of the byte that failed, or -1. */
static int oled_init_panel(void)
{
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
        if (oled_cmd(init_seq[i]) != ESP_OK) return (int)i;
    }
    return -1;
}

esp_err_t display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_OLED_SDA,
        .scl_io_num = BOARD_OLED_SCL,
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

    /* A panel can be left mid-transfer by a reset -- a panic, a watchdog, an
     * OTA reboot -- and then NACKs partway through init until it loses power.
     * That is not a reason to need a physical power cycle: clock the bus free
     * (i2c_master_bus_reset sends the nine recovery clocks) and try again. */
    int failed = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        failed = oled_init_panel();
        if (failed < 0) break;
        ESP_LOGW(TAG, "OLED init failed at byte %d, resetting the bus (attempt %d)",
                 failed, attempt + 1);
        i2c_master_bus_reset(s_bus);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (failed >= 0) {
        ESP_LOGW(TAG, "OLED did not initialise, continuing without display");
        return ESP_OK;
    }

    s_ready = true;
    fb_clear();
    oled_flush();
    ESP_LOGI(TAG, "OLED ready on SDA %d SCL %d",
             BOARD_OLED_SDA, BOARD_OLED_SCL);
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
