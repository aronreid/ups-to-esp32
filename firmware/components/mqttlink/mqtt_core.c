/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MQTT without a network: settings, topics, commands and payloads. See
 * mqtt_core.h. Every payload is built with snprintf into a caller's buffer
 * and checked for truncation; test_mqtt.py parses each one.
 */
#include "mqtt_core.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void mqtt_cfg_defaults(mqtt_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = MQTT_PORT_DEF;
    c->discovery = true;
}

static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) return false;
    }
    return true;
}

bool mqtt_cfg_check(const mqtt_cfg_t *c, char *why, size_t why_len)
{
    size_t hl = strnlen(c->host, MQTT_HOST_MAX);
    if (hl >= MQTT_HOST_MAX || strnlen(c->user, MQTT_USER_MAX) >= MQTT_USER_MAX ||
        strnlen(c->pass, MQTT_PASS_MAX) >= MQTT_PASS_MAX) {
        if (why) snprintf(why, why_len, "a field is too long");
        return false;
    }
    for (size_t i = 0; i < hl; i++) {
        char ch = c->host[i];
        if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-') {
            if (why) snprintf(why, why_len, "the broker must be a host name or an IPv4 address");
            return false;
        }
    }
    if (c->enabled && hl == 0) {
        if (why) snprintf(why, why_len, "enter the broker's address");
        return false;
    }
    if (c->port == 0) {
        if (why) snprintf(why, why_len, "the port must be 1 to 65535");
        return false;
    }
    if (!printable(c->user) || !printable(c->pass)) {
        if (why) snprintf(why, why_len, "the user name and password must be plain text");
        return false;
    }
    return true;
}

/* ---- commands ------------------------------------------------------------ */

static bool eq_ci(const char *a, int alen, const char *b)
{
    int bl = (int)strlen(b);
    if (alen != bl) return false;
    for (int i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

mqtt_cmd_t mqtt_parse(const char *base, const char *topic, int topic_len,
                      const char *data, int data_len, int *idx, bool *on)
{
    static const char ha[] = MQTT_DISC_PREFIX "/status";
    if (topic_len == (int)sizeof(ha) - 1 && !memcmp(topic, ha, sizeof(ha) - 1)) {
        return eq_ci(data, data_len, "online") ? MQTT_CMD_HA_ONLINE : MQTT_CMD_NONE;
    }
    int bl = (int)strlen(base);
    static const char mid[] = "/output/";
    int ml = (int)sizeof(mid) - 1;
    if (topic_len < bl + ml + 1 || memcmp(topic, base, bl) || memcmp(topic + bl, mid, ml)) {
        return MQTT_CMD_NONE;
    }
    const char *p = topic + bl + ml;
    const char *end = topic + topic_len;
    if (p >= end || *p < '1' || *p > '0' + OUT_N) return MQTT_CMD_NONE;
    int n = *p - '1';
    p++;
    int rest = (int)(end - p);
    if (rest == 4 && !memcmp(p, "/set", 4)) {
        if (eq_ci(data, data_len, "ON")) *on = true;
        else if (eq_ci(data, data_len, "OFF")) *on = false;
        else return MQTT_CMD_NONE;
        *idx = n;
        return MQTT_CMD_SET;
    }
    if (rest == 6 && !memcmp(p, "/press", 6)) {
        if (!eq_ci(data, data_len, "PRESS")) return MQTT_CMD_NONE;
        *idx = n;
        return MQTT_CMD_PRESS;
    }
    return MQTT_CMD_NONE;
}

/* ---- payloads ------------------------------------------------------------ */

/* s, escaped for a JSON string, into out. */
static const char *jesc(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch < 0x20 || ch > 0x7e) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch < 0x80 ? ch : '?'); }
        else out[o++] = (char)ch;
    }
    out[o] = '\0';
    return out;
}

static int fin(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

static void num(char *out, size_t cap, float v, int decimals)
{
    if (v <= MQTT_ABSENT * 0.5f) snprintf(out, cap, "null");
    else snprintf(out, cap, "%.*f", decimals, (double)v);
}

int mqtt_ups_json(const mqtt_ups_t *u, char *out, size_t cap)
{
    char st[112], mdl[96], c[16], r[16], l[16], iv[16], ov[16], bv[16], pw[16];
    num(c, sizeof(c), u->charge, 0);
    num(r, sizeof(r), u->runtime, 0);
    num(l, sizeof(l), u->load, 0);
    num(iv, sizeof(iv), u->in_v, 1);
    num(ov, sizeof(ov), u->out_v, 1);
    num(bv, sizeof(bv), u->batt_v, 2);
    num(pw, sizeof(pw), u->power, 0);
    char sts[120];
    if (u->have_status) snprintf(sts, sizeof(sts), "\"%s\"", jesc(u->status, st, sizeof(st)));
    else snprintf(sts, sizeof(sts), "null");
    int n = snprintf(out, cap,
                     "{\"status\":%s,\"on_battery\":%s,\"low_battery\":%s,\"charge\":%s,"
                     "\"runtime\":%s,\"load\":%s,\"input_voltage\":%s,\"output_voltage\":%s,"
                     "\"battery_voltage\":%s,\"power\":%s,\"power_estimated\":%s,\"model\":\"%s\"}",
                     sts, u->on_battery ? "true" : "false", u->low_battery ? "true" : "false",
                     c, r, l, iv, ov, bv, pw, u->power_est ? "true" : "false",
                     jesc(u->model, mdl, sizeof(mdl)));
    return fin(n, cap);
}

/* The device block and availability, shared by every entity. */
static int dev_tail(const mqtt_dev_t *d, char *out, size_t cap)
{
    char nm[96], mdl[96], sw[64], url[96];
    jesc(d->name, nm, sizeof(nm));
    jesc(d->model, mdl, sizeof(mdl));
    jesc(d->sw, sw, sizeof(sw));
    jesc(d->url ? d->url : "", url, sizeof(url));
    int n = snprintf(out, cap,
                     "\"availability_topic\":\"%s/status\",\"device\":{\"identifiers\":[\"%s\"],"
                     "\"name\":\"%s\",\"manufacturer\":\"My UPS Pal\",\"model\":\"%s\","
                     "\"sw_version\":\"%s\"%s%s%s}}",
                     d->base, d->id, nm, mdl, sw,
                     url[0] ? ",\"configuration_url\":\"" : "", url, url[0] ? "\"" : "");
    return fin(n, cap);
}

typedef struct {
    const char *object, *component, *name, *tmpl, *unit, *dev_class, *icon;
    bool measurement;
} ups_ent_t;

static const ups_ent_t UPS_ENT[] = {
    { "status", "sensor", "Status", "{{ value_json.status }}", NULL, NULL, "mdi:power-plug", false },
    { "charge", "sensor", "Battery charge", "{{ value_json.charge }}", "%", "battery", NULL, true },
    { "runtime", "sensor", "Runtime", "{{ value_json.runtime }}", "s", "duration", NULL, true },
    { "load", "sensor", "Load", "{{ value_json.load }}", "%", NULL, "mdi:gauge", true },
    { "power", "sensor", "Power", "{{ value_json.power }}", "W", "power", NULL, true },
    { "input_voltage", "sensor", "Input voltage", "{{ value_json.input_voltage }}", "V", "voltage", NULL, true },
    { "output_voltage", "sensor", "Output voltage", "{{ value_json.output_voltage }}", "V", "voltage", NULL, true },
    { "battery_voltage", "sensor", "Battery voltage", "{{ value_json.battery_voltage }}", "V", "voltage", NULL, true },
    { "on_battery", "binary_sensor", "On battery",
      "{{ 'ON' if value_json.on_battery else 'OFF' }}", NULL, NULL, "mdi:battery-alert", false },
    { "low_battery", "binary_sensor", "Low battery",
      "{{ 'ON' if value_json.low_battery else 'OFF' }}", NULL, "battery", NULL, false },
};

int mqtt_ups_entity_count(void) { return (int)(sizeof(UPS_ENT) / sizeof(UPS_ENT[0])); }

int mqtt_ups_disc(const mqtt_dev_t *d, int k, char *topic, size_t tcap, char *out, size_t cap)
{
    if (k < 0 || k >= mqtt_ups_entity_count()) return -1;
    const ups_ent_t *e = &UPS_ENT[k];
    if (fin(snprintf(topic, tcap, MQTT_DISC_PREFIX "/%s/%s/%s/config", e->component, d->id, e->object),
            tcap) < 0) return -1;
    char extra[160] = "";
    size_t x = 0;
    if (e->unit) x += (size_t)snprintf(extra + x, sizeof(extra) - x, "\"unit_of_measurement\":\"%s\",", e->unit);
    if (e->dev_class) x += (size_t)snprintf(extra + x, sizeof(extra) - x, "\"device_class\":\"%s\",", e->dev_class);
    if (e->icon) x += (size_t)snprintf(extra + x, sizeof(extra) - x, "\"icon\":\"%s\",", e->icon);
    if (e->measurement) snprintf(extra + x, sizeof(extra) - x, "\"state_class\":\"measurement\",");
    int n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s/ups\","
                     "\"value_template\":\"%s\",%s",
                     e->name, d->id, e->object, d->base, e->tmpl, extra);
    if (fin(n, cap) < 0) return -1;
    int t = dev_tail(d, out + n, cap - (size_t)n);
    return t < 0 ? -1 : n + t;
}

static const char *const OUT_COMP[3] = { "switch", "button", "binary_sensor" };
static const char *const OUT_SUFFIX[3] = { "sw", "bt", "bs" };

int mqtt_out_disc(const mqtt_dev_t *d, int i, const out_pin_cfg_t *p, bool control, int comp,
                  char *topic, size_t tcap, char *out, size_t cap)
{
    if (comp < 0 || comp > 2 || i < 0 || i >= OUT_N) return -1;
    if (fin(snprintf(topic, tcap, MQTT_DISC_PREFIX "/%s/%s/output%d/config", OUT_COMP[comp], d->id, i + 1),
            tcap) < 0) return -1;
    int want = -1;
    if (p->mode == OUT_MODE_SWITCH) want = control ? 0 : 2;
    else if (p->mode == OUT_MODE_BUTTON) want = control ? 1 : 2;
    if (want != comp) {
        if (cap) out[0] = '\0';
        return 0;
    }
    char nm[OUT_NAME_MAX * 2];
    jesc(p->name, nm, sizeof(nm));
    int n;
    if (comp == 0) {
        n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"unique_id\":\"%s_out%d_%s\",\"state_topic\":\"%s/output/%d\","
                     "\"command_topic\":\"%s/output/%d/set\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
                     "\"icon\":\"mdi:electric-switch\",",
                     nm, d->id, i + 1, OUT_SUFFIX[comp], d->base, i + 1, d->base, i + 1);
    } else if (comp == 1) {
        n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"unique_id\":\"%s_out%d_%s\","
                     "\"command_topic\":\"%s/output/%d/press\",\"payload_press\":\"PRESS\","
                     "\"icon\":\"mdi:gesture-tap-button\",",
                     nm, d->id, i + 1, OUT_SUFFIX[comp], d->base, i + 1);
    } else {
        n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"unique_id\":\"%s_out%d_%s\",\"state_topic\":\"%s/output/%d\","
                     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",",
                     nm, d->id, i + 1, OUT_SUFFIX[comp], d->base, i + 1);
    }
    if (fin(n, cap) < 0) return -1;
    int t = dev_tail(d, out + n, cap - (size_t)n);
    return t < 0 ? -1 : n + t;
}
