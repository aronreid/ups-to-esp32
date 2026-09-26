/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Board abstraction for the UPS to ESP32 Module.
 *
 * Rev A is the only target: it is the board that is made. Firmware was brought
 * up on a Freenove devkit before Rev A existed; that target has been removed.
 *
 * Every pin the application touches is named here. Application code must not
 * contain GPIO numbers, so that a Rev B is a new block below and nothing else.
 *
 * GPIO19/20 (USB D-/D+) are deliberately absent. They are fixed by silicon and
 * are owned by the usb_host peripheral, not by us.
 */
#ifndef UPSA_BOARD_H
#define UPSA_BOARD_H

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#if defined(CONFIG_UPSA_BOARD_REV_A)

#define BOARD_NAME              "UPS to ESP32 Module Rev A"
/* Picks the OTA asset: ups-adaptor-reva.bin. The two targets have different
 * pin maps, so installing the wrong one is not a cosmetic mistake. */
#define BOARD_OTA_TARGET        "reva"
/* What an image built FOR THIS BOARD calls itself in its app descriptor. The
 * OTA client refuses anything else, so a mislabelled asset cannot be
 * installed -- which has happened. */
#define BOARD_IMAGE_NAME        "ups-adaptor-reva"
/* Rev A: see docs/hardware.md pin assignment table. */
#define BOARD_PIN_VBUS_EN       GPIO_NUM_4   /* AP2171W EN, active high        */
/* Rev A uses an AP2171W: a SOT-23-5 load switch fits EN plus exactly one of
 * ILIM or FLAG, and this part chose FLAG. So fault reporting is back and the
 * current limit is fixed by the part rather than set by a resistor.
 * See docs/hardware-schematic.md section 1. */
#define BOARD_PIN_VBUS_FAULT    GPIO_NUM_5   /* AP2171W FLG, open drain, active low */
#define BOARD_PIN_LED_STATUS    GPIO_NUM_48  /* green, plain LED, 1k series    */
#define BOARD_PIN_LED_FAULT     GPIO_NUM_47  /* red, plain LED, 1k series      */
#define BOARD_PIN_I2C_SDA       GPIO_NUM_8
#define BOARD_PIN_I2C_SCL       GPIO_NUM_9
#define BOARD_PIN_FACTORY_BTN   GPIO_NUM_0   /* BOOT button, doubles as reset  */
#define BOARD_HAS_VBUS_SWITCH   1
#define BOARD_HAS_VBUS_FAULT    1
#define BOARD_HAS_STATUS_LEDS   1
/* A display on Rev A only with a VCC-first module; Kconfig hides every other
 * way, and this catches an edit to it. */
#if defined(CONFIG_UPSA_HAVE_OLED) && !defined(CONFIG_UPSA_REVA_OLED_VCC_FIRST)
#error "No OLED on Rev A except a VCC-first module: J3 powers a standard one backwards (Kconfig UPSA_REVA_OLED_VCC_FIRST)"
#endif
#if defined(CONFIG_UPSA_REVA_OLED_VCC_FIRST)
/* J3 pin 3 is the board's SDA net (GPIO8) and pin 4 its SCL (GPIO9); a
 * VCC-first module puts SCL on pin 3 and SDA on pin 4. The board's nets are
 * unchanged -- BOARD_PIN_I2C_* still describe them -- only the display swaps. */
#define BOARD_OLED_SDA          BOARD_PIN_I2C_SCL
#define BOARD_OLED_SCL          BOARD_PIN_I2C_SDA
#endif

#else
#error "No target board selected. Run idf.py menuconfig -> UPS to ESP32 Module board."
#endif

/* The pins the DISPLAY uses for SDA and SCL. The board's own I2C nets, unless
 * a board block above says the module in its socket crosses them. */
#ifndef BOARD_OLED_SDA
#define BOARD_OLED_SDA          BOARD_PIN_I2C_SDA
#define BOARD_OLED_SCL          BOARD_PIN_I2C_SCL
#endif

/* Bring up board-owned GPIO. Call once, first, from app_main. */
esp_err_t board_init(void);

/* Assert or deassert VBUS on the USB-A host port.
 * Returns ESP_ERR_NOT_SUPPORTED on boards without a load switch, so callers
 * can log-and-continue rather than treating it as fatal. */
esp_err_t board_vbus_set(bool on);

/* Drop VBUS for CONFIG_UPSA_VBUS_CYCLE_MS, then restore it. Blocks.
 * This is the recovery path for a wedged UPS USB interface. */
esp_err_t board_vbus_cycle(void);

/* True if the load switch is reporting an overcurrent or thermal fault.
 * Always false where the board has no FLAG line. */
bool board_vbus_fault(void);

/* True while the factory-reset button is held (active low). */
bool board_factory_button_pressed(void);

/* Coarse device state, shown on two plain LEDs.
 *
 * Semantic rather than per-pin: Rev A has a green and a red LED, a later board
 * may not, and callers should not know or care. Deliberately coarse -- the OLED
 * and web UI carry anything that needs detail.
 *
 * THE OLED MUST SHOW EVERY STATE HERE. On Rev A the display module sits
 * directly over both LEDs, so with it fitted the LEDs cannot be seen and the
 * screen is the only local indicator. display.c renders board_status_get()
 * through a switch with no default, so a state added here and not there is a
 * -Wswitch error, and tools/test/test_display_parity.py checks the same thing
 * without a build. */
typedef enum {
    BOARD_STATUS_BOOTING,       /* green solid                    */
    BOARD_STATUS_PROVISIONING,  /* green fast blink: setup AP up  */
    BOARD_STATUS_CONNECTING,    /* green slow blink               */
    BOARD_STATUS_ONLINE,        /* green solid, red off           */
    BOARD_STATUS_NO_UPS,        /* green solid, red slow blink    */
    BOARD_STATUS_FAULT,         /* red solid: VBUS fault          */
} board_status_t;

void board_status_set(board_status_t state);

/* The state last set, for the OLED to mirror what the LEDs are showing. */
board_status_t board_status_get(void);

#endif /* UPSA_BOARD_H */
