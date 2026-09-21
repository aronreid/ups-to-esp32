/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wi-Fi lifecycle and provisioning state.
 *
 * Two modes, chosen automatically:
 *   - Credentials in NVS  -> STA, connect, and reconnect forever on dropout.
 *   - No credentials, or  -> SoftAP "ups-adaptor-XXXX" + captive portal, so
 *     factory button held     setup happens in a browser with no app and no
 *                             serial console.
 *
 * Reconnection is not optional. Several existing ESP32 NUT servers never
 * recover from an AP reboot and need a physical reset; a device in a closet
 * behind a UPS must not have that failure mode (CLAUDE.md).
 */
#ifndef UPSA_NETMGR_H
#define UPSA_NETMGR_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    NETMGR_BOOTING,
    NETMGR_AP_PROVISIONING,  /* SoftAP up, waiting for setup */
    NETMGR_STA_CONNECTING,
    NETMGR_STA_CONNECTED,
    NETMGR_STA_RETRYING,     /* lost the AP, backing off and retrying */
} netmgr_state_t;

esp_err_t netmgr_start(void);
netmgr_state_t netmgr_state(void);

/* Store credentials, then reboot into STA a beat later so the HTTP reply gets
 * out first. Called by the web UI setup form. */
esp_err_t netmgr_provision(const char *ssid, const char *pass);

/* The provisioning AP's name, or "" when not provisioning. The setup page
 * shows it so someone can tell which of two boards they are talking to. */
const char *netmgr_ap_ssid(void);

/* The network this device is on, or trying to join; "" before either. */
const char *netmgr_ssid(void);

/* Associated signal strength in dBm, or 0 when not associated. */
int netmgr_rssi(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} netmgr_ap_t;

/* Visible networks, strongest first, duplicates collapsed. Blocks for a couple
 * of seconds. Returns how many were written, or -1. */
int netmgr_scan(netmgr_ap_t *out, int max);

/* Wipe stored credentials and reboot into SoftAP provisioning. */
esp_err_t netmgr_factory_reset(void);

/* Dotted-quad of the current address, or "0.0.0.0". For the status page. */
const char *netmgr_ip(void);

#endif /* UPSA_NETMGR_H */
