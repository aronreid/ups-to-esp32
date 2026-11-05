/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The board name and the UPS name an owner can set, shared by the Wi-Fi and
 * the Ethernet netmgr. Both are optional: unset, the board is
 * ups-esp32-XXXX and the UPS is "ups", as every board has always been, so
 * NUT and Home Assistant setups made before this keep working.
 *
 * Read once at start; changing either is store-and-restart, because the
 * hostname is handed to DHCP, mDNS, the certificate and Tailscale at start
 * and NUT clients hold the UPS name per connection. */

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mdns.h"
#include "lwip/ip4_addr.h"
#include "netmgr.h"
#include "netmgr_name.h"

static const char *TAG = "netmgr";

#define NVS_NS            "upsa"
#define NVS_KEY_HOSTNAME  "hostname"
#define NVS_KEY_UPSNAME   "upsname"

static char s_default_host[NETMGR_NAME_MAX + 1] = "ups-esp32";
static char s_custom_host[NETMGR_NAME_MAX + 1];      /* "" = the default */
static char s_ups[NETMGR_NAME_MAX + 1] = "ups";
static char s_custom_ups[NETMGR_NAME_MAX + 1];       /* "" = "ups" */
static bool s_alias_added;
static esp_timer_handle_t s_restart_timer;

static void load_one(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

void netmgr_names_load(const char *default_host, char *host_out, size_t cap)
{
    strlcpy(s_default_host, default_host, sizeof(s_default_host));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        load_one(h, NVS_KEY_HOSTNAME, s_custom_host, sizeof(s_custom_host));
        load_one(h, NVS_KEY_UPSNAME, s_custom_ups, sizeof(s_custom_ups));
        nvs_close(h);
    }
    /* A stored name that fails today's checks is ignored, not trusted. */
    if (s_custom_host[0] && !netmgr_hostname_check(s_custom_host, NULL, 0)) {
        ESP_LOGE(TAG, "stored board name ignored");
        s_custom_host[0] = '\0';
    }
    if (s_custom_ups[0] && !netmgr_upsname_check(s_custom_ups, NULL, 0)) {
        ESP_LOGE(TAG, "stored UPS name ignored");
        s_custom_ups[0] = '\0';
    }
    strlcpy(s_ups, s_custom_ups[0] ? s_custom_ups : "ups", sizeof(s_ups));
    strlcpy(host_out, s_custom_host[0] ? s_custom_host : s_default_host, cap);
    if (s_custom_host[0]) {
        ESP_LOGI(TAG, "board name %s (also answers to %s.local)", s_custom_host, s_default_host);
    }
}

const char *netmgr_default_hostname(void) { return s_default_host; }
const char *netmgr_ups_name(void) { return s_ups; }

void netmgr_names_get(char *host, size_t host_cap, char *ups, size_t ups_cap)
{
    strlcpy(host, s_custom_host, host_cap);
    strlcpy(ups, s_custom_ups, ups_cap);
}

static void restart_cb(void *arg) { esp_restart(); }

esp_err_t netmgr_names_set(const char *host, const char *ups, char *why, size_t why_len)
{
    char h[NETMGR_NAME_MAX + 1] = "", u[NETMGR_NAME_MAX + 1] = "";
    if (host && strlen(host) > NETMGR_NAME_MAX) {
        snprintf(why, why_len, "the board name must be 1 to %d characters", NETMGR_NAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (ups && strlen(ups) > NETMGR_NAME_MAX) {
        snprintf(why, why_len, "the UPS name must be 1 to %d characters", NETMGR_NAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (host) strlcpy(h, host, sizeof(h));
    if (ups) strlcpy(u, ups, sizeof(u));
    if (h[0] && !netmgr_hostname_check(h, why, why_len)) return ESP_ERR_INVALID_ARG;
    if (u[0] && !netmgr_upsname_check(u, why, why_len)) return ESP_ERR_INVALID_ARG;
    /* The default typed back in is the default, not a custom name equal to
     * it (which would add a second mDNS record for the same name). */
    if (strcasecmp(h, s_default_host) == 0) h[0] = '\0';
    if (strcmp(u, "ups") == 0) u[0] = '\0';

    nvs_handle_t nh;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nh);
    if (err != ESP_OK) {
        snprintf(why, why_len, "could not open settings storage");
        return err;
    }
    if (h[0]) nvs_set_str(nh, NVS_KEY_HOSTNAME, h); else nvs_erase_key(nh, NVS_KEY_HOSTNAME);
    if (u[0]) nvs_set_str(nh, NVS_KEY_UPSNAME, u);  else nvs_erase_key(nh, NVS_KEY_UPSNAME);
    err = nvs_commit(nh);
    nvs_close(nh);
    if (err != ESP_OK) {
        snprintf(why, why_len, "could not save the names");
        return err;
    }
    ESP_LOGI(TAG, "names saved: board %s, UPS %s; restarting",
             h[0] ? h : s_default_host, u[0] ? u : "ups");
    /* Restart after the reply has gone. */
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "names_restart" };
        if (esp_timer_create(&a, &s_restart_timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, 1500 * 1000);
    return ESP_OK;
}

/* With a board name set, ups-esp32-XXXX.local keeps answering, at the same
 * address: the NUT and Home Assistant setups made before the rename name it.
 * Called once mDNS is up and again on every new address. */
void netmgr_names_mdns_alias(uint32_t ip4_addr)
{
    if (!s_custom_host[0] || !ip4_addr) return;
    mdns_ip_addr_t a = { 0 };
    a.addr.type = ESP_IPADDR_TYPE_V4;
    a.addr.u_addr.ip4.addr = ip4_addr;
    esp_err_t err = s_alias_added ? mdns_delegate_hostname_set_address(s_default_host, &a)
                                  : mdns_delegate_hostname_add(s_default_host, &a);
    if (err == ESP_OK) s_alias_added = true;
    else ESP_LOGW(TAG, "could not add %s.local as a second name (%s)", s_default_host,
                  esp_err_to_name(err));
}
