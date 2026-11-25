/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Optional SSD1306 status display, 128x64 over I2C.
 *
 * Entirely compiled out unless CONFIG_UPSA_HAVE_OLED is set, so a board built
 * without the module costs nothing in flash or RAM and every call below becomes
 * a no-op. Application code calls these unconditionally -- no #ifdef at the
 * call site.
 *
 * This is a debug and field-diagnostic aid, not the primary UI. The web
 * interface is. The screen's most useful job is showing the IP address, so the
 * board can be reached without hunting through a DHCP table.
 */
#ifndef UPSA_DISPLAY_H
#define UPSA_DISPLAY_H

#include "esp_err.h"

/* Bring up I2C and the panel. Returns ESP_OK (and does nothing) when the OLED
 * is disabled at build time, so callers need not special-case it. */
esp_err_t display_init(void);

/* Start the refresh task. Redraws roughly once a second from the shared
 * ups_data_t snapshot and netmgr state. */
esp_err_t display_start(void);

/* Draw an arbitrary line immediately, for bring-up messages before the normal
 * refresh loop is meaningful. row is 0..7 (8 pixel rows of text). */
void display_line(int row, const char *text);

#endif /* UPSA_DISPLAY_H */
