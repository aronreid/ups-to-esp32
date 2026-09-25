/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NUT (Network UPS Tools) server, TCP 3493.
 *
 * Implements the read-only subset of the NUT protocol that monitoring clients
 * actually use: Home Assistant's NUT integration, Synology, TrueNAS, unraid
 * and upsmon. See docs/firmware.md for the exact command list.
 */
#ifndef UPSA_NUT_SERVER_H
#define UPSA_NUT_SERVER_H

#include "esp_err.h"

#define NUT_DEFAULT_PORT 3493

/* Start the listener. Safe to call before Wi-Fi is up: the socket binds to
 * INADDR_ANY and simply accepts nothing until an interface exists. */
esp_err_t nut_server_start(uint16_t port);

/* Clients connected right now, and the name they must address. Both are shown
 * on the status page so the details a NAS needs can be read off the device
 * rather than guessed. */
int nut_server_clients(void);
const char *nut_server_ups_name(void);

#endif /* UPSA_NUT_SERVER_H */
