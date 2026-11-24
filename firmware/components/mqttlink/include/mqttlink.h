/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MQTT: the UPS's readings and the Outputs, published to the owner's broker
 * with Home Assistant discovery, so the UPS and every output show up in Home
 * Assistant (and Node-RED, openHAB, ...) with no YAML. A second output beside
 * NUT, never instead of it.
 *
 * Off until the owner turns it on and enters a broker. Operating an output
 * from MQTT is a separate setting (Outputs, "MQTT may operate the outputs"),
 * off by default: anyone who can publish to the broker could press. Plain
 * MQTT on the owner's own network; the password is stored like the SNMP
 * community and never sent back to the page.
 */
#ifndef UPSA_MQTTLINK_H
#define UPSA_MQTTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "mqtt_core.h"

/* Read the settings and, if on, connect. Never fatal. */
esp_err_t mqttlink_start(void);

/* The settings as saved, password included: the page must not send it out. */
void mqttlink_get_cfg(mqtt_cfg_t *out);

/* Check, save and apply (reconnects). An empty password in `c` keeps the
 * saved one unless clear_pass. */
esp_err_t mqttlink_set_cfg(const mqtt_cfg_t *c, bool clear_pass, char *why, size_t why_len);

typedef struct {
    bool     enabled;
    bool     connected;
    uint32_t connects;        /* successful connections since boot */
    uint32_t published;       /* messages sent */
    uint32_t commands;        /* output commands received */
    char     last_error[64];  /* "" if none since the last connection */
    char     base[MQTT_BASE_MAX];
} mqttlink_status_t;

void mqttlink_get_status(mqttlink_status_t *out);

#endif /* UPSA_MQTTLINK_H */
