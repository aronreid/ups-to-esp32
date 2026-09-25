/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Board abstraction for ups-adaptor.
 *
 * Rev A is the reference target. The Freenove devkit exists so firmware can run
 * before Rev A boards are fabricated, and is not a design constraint.
 *
 * Every pin the application touches is named here. Application code must not
 * contain GPIO numbers: the Freenove bring-up board and the Rev A PCB differ,
 * and both are built from this one source tree.
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

#define BOARD_NAME              "ups-adaptor Rev A"
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
/* Kconfig already hides the option for Rev A; this catches an edit to it. */
#if defined(CONFIG_UPSA_HAVE_OLED)
#error "No OLED on Rev A: its header is wired in reverse to standard modules (Kconfig UPSA_BOARD_OLED_OK)"
#endif

#elif defined(CONFIG_UPSA_BOARD_FREENOVE)

#define BOARD_NAME              "Freenove ESP32-S3-WROOM devkit"
/* Picks the OTA asset: ups-adaptor-freenove.bin. See the Rev A note above --
 * the two targets have different pin maps. */
#define BOARD_OTA_TARGET        "freenove"
/* See BOARD_IMAGE_NAME above. */
#define BOARD_IMAGE_NAME        "ups-adaptor-freenove"
/* The devkit has no load switch: VBUS on the pigtail is hardwired to 5V.
 * ups_hid must therefore degrade gracefully when a power-cycle is requested,
 * which is also what exercises that path before Rev A silicon exists. */
#define BOARD_PIN_VBUS_EN       GPIO_NUM_NC
#define BOARD_PIN_VBUS_FAULT    GPIO_NUM_NC
/* Development fallback only. Lacks the VBUS load switch and the status LEDs,
 * so those paths compile out; the UART console and the optional OLED cover
 * bring-up. Do not let this target's limitations shape design decisions --
 * Rev A is the reference. */
#define BOARD_PIN_LED_STATUS    GPIO_NUM_NC
#define BOARD_PIN_LED_FAULT     GPIO_NUM_NC
/* Not 8/9 as on Rev A: the devkit does not expose them free (on Freenove's
 * camera variant they are camera data lines). 1 and 2 have no strapping role. */
#define BOARD_PIN_I2C_SDA       GPIO_NUM_1
#define BOARD_PIN_I2C_SCL       GPIO_NUM_2
#define BOARD_PIN_FACTORY_BTN   GPIO_NUM_0
#define BOARD_HAS_VBUS_SWITCH   0
#define BOARD_HAS_VBUS_FAULT    0
#define BOARD_HAS_STATUS_LEDS   0

#else
#error "No target board selected. Run idf.py menuconfig -> ups-adaptor board."
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
 * Semantic rather than per-pin: Rev A has a green and a red LED, the devkit has
 * neither, and callers should not know or care. Deliberately coarse -- the OLED
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
