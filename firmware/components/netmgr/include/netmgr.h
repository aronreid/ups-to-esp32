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

/* Called by the web UI setup form. Two behaviours:
 *
 *   From the setup AP: join the network WITHOUT restarting, while the AP stays
 *   up, so the page the person is looking at can tell them the address the
 *   router handed out -- or that the password was wrong -- before they leave.
 *   Credentials are stored only once an address arrives. Progress is read
 *   with netmgr_join_state(). Returns ESP_ERR_INVALID_STATE mid-attempt.
 *
 *   Already on a network: store, then reboot into STA a beat later so the
 *   HTTP reply gets out first. */
esp_err_t netmgr_provision(const char *ssid, const char *pass);

typedef enum {
    NETMGR_JOIN_IDLE,
    NETMGR_JOIN_TRYING,
    NETMGR_JOIN_OK,       /* has an address; setup AP closes by restart soon */
    NETMGR_JOIN_FAILED,   /* see netmgr_join_error(); the form can retry */
} netmgr_join_t;

netmgr_join_t netmgr_join_state(void);
/* The address the network gave during a setup join, "" until it has one. */
const char *netmgr_join_ip(void);
/* Why the last setup join failed, in words for a person; "" if it has not. */
const char *netmgr_join_error(void);

/* This board's own name: ups-adaptor-XXXX, the last four hex digits of its
 * AP MAC. The setup AP, the mDNS name and the DHCP hostname all use it, so
 * two boards on one network never answer to the same name and the one on a
 * sticker is the one in the router's client list. */
const char *netmgr_hostname(void);

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
