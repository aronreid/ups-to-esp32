/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MQTT, the parts that do not need a network: what a setting may be, the
 * topics, what a command message means, and the JSON the board publishes
 * (Home Assistant's discovery and the readings). Free of ESP-IDF so
 * tools/test/test_mqtt.py compiles it on the host and checks every payload
 * with a real JSON parser.
 *
 * Topics, under base = "myupspal/<host>":
 *   <base>/status            "online" / "offline" (retained; offline is the will)
 *   <base>/ups               the readings, JSON (retained)
 *   <base>/output/<n>        "ON" / "OFF" (retained), n = 1..4
 *   <base>/output/<n>/set    "ON" / "OFF"  -> a Switch      (only if allowed)
 *   <base>/output/<n>/press  "PRESS"       -> a Button      (only if allowed)
 *   homeassistant/<component>/<id>/<object>/config   discovery (retained)
 *   homeassistant/status     Home Assistant's birth: "online" means send discovery again
 */
#ifndef UPSA_MQTT_CORE_H
#define UPSA_MQTT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "outputs_core.h"

#define MQTT_HOST_MAX   64
#define MQTT_USER_MAX   64
#define MQTT_PASS_MAX   64
#define MQTT_PORT_DEF   1883
#define MQTT_BASE_MAX   80
#define MQTT_DISC_PREFIX "homeassistant"

typedef struct {
    bool     enabled;
    bool     discovery;               /* Home Assistant discovery */
    uint16_t port;
    char     host[MQTT_HOST_MAX];
    char     user[MQTT_USER_MAX];
    char     pass[MQTT_PASS_MAX];
} mqtt_cfg_t;

void mqtt_cfg_defaults(mqtt_cfg_t *c);
bool mqtt_cfg_check(const mqtt_cfg_t *c, char *why, size_t why_len);

/* What a received message asks for. */
typedef enum {
    MQTT_CMD_NONE = 0,     /* not ours, or not understood */
    MQTT_CMD_SET,          /* a Switch: on = arg */
    MQTT_CMD_PRESS,        /* a Button */
    MQTT_CMD_HA_ONLINE,    /* Home Assistant restarted: send discovery again */
} mqtt_cmd_t;

/* Parse one message. `topic` and `data` are not NUL-terminated. idx is the
 * output, 0-based; on is the Switch state asked for. */
mqtt_cmd_t mqtt_parse(const char *base, const char *topic, int topic_len,
                      const char *data, int data_len, int *idx, bool *on);

/* What the readings message holds. Floats < -1e30 are absent (published as
 * null, which Home Assistant shows as unknown, never a made-up 0). */
#define MQTT_ABSENT (-1e38f)
typedef struct {
    char  status[48];          /* NUT's ups.status, "" if not known */
    bool  have_status;
    bool  on_battery;
    bool  low_battery;
    float charge, runtime, load, in_v, out_v, batt_v, power;
    bool  power_est;
    char  model[40];
} mqtt_ups_t;

/* Each returns the length written, or -1 if `cap` was too small. */
int mqtt_ups_json(const mqtt_ups_t *u, char *out, size_t cap);

/* The device every entity belongs to. */
typedef struct {
    const char *id;        /* "myupspal_<12 hex of the MAC>" */
    const char *name;      /* the board's host name */
    const char *model;     /* BOARD_NAME */
    const char *sw;        /* firmware version */
    const char *url;       /* "http://<ip>/" or "" */
    const char *base;      /* the topic base */
} mqtt_dev_t;

/* The UPS's entities, k = 0 .. mqtt_ups_entity_count() - 1. */
int mqtt_ups_entity_count(void);
int mqtt_ups_disc(const mqtt_dev_t *d, int k, char *topic, size_t tcap, char *out, size_t cap);

/* Output i's entities. Its kind decides the component: a Switch is a switch,
 * a Button a button, either one read-only (control off) a binary_sensor, an
 * Off output nothing. comp = 0 switch, 1 button, 2 binary_sensor: the caller
 * publishes all three, and an empty payload (out[0] = 0, returns 0) for the
 * ones that do not apply, which removes any entity left from a previous
 * setting. */
int mqtt_out_disc(const mqtt_dev_t *d, int i, const out_pin_cfg_t *p, bool control, int comp,
                  char *topic, size_t tcap, char *out, size_t cap);

#endif /* UPSA_MQTT_CORE_H */
