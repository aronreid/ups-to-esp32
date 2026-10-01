/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HTTP setup and status interface. The primary UI for this device: there is no
 * app, no serial console requirement, and the OLED is optional.
 *
 * Routes:
 *   GET  /            single-page UI, embedded in flash
 *   GET  /api/status  JSON snapshot: UPS readings, link state, Wi-Fi, version
 *   GET  /api/scan    JSON list of visible APs, for the setup form
 *   POST /api/wifi    {ssid, pass} -> store and reconnect
 *   POST /api/ups/reset  power-cycle the UPS USB interface
 *   POST /api/factory factory reset
 *   POST /api/ota     firmware image upload
 */
#ifndef UPSA_WEBUI_H
#define UPSA_WEBUI_H

#include "esp_err.h"

esp_err_t webui_start(void);

#endif /* UPSA_WEBUI_H */
