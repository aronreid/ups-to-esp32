/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The netmgr.h API over wired Ethernet, for boards with no Wi-Fi radio
 * (UPSA_NET_ETHERNET). netmgr.c is the Wi-Fi implementation; exactly one of
 * the two is built.
 *
 * Nothing to provision: a cable and DHCP are the whole setup, so the setup
 * access point, the join form and the scan do not exist here and report
 * "not supported". What carries over is what matters in a closet: a fixed
 * address that must prove itself against the gateway (falling back to DHCP
 * if it does not), the .local name, and a reboot if the board has been off
 * the network for fifteen minutes.
 *
 * The state names stay the STA_* ones so the status page, the LED/OLED
 * mapping and the NUT supervisor need no second vocabulary: CONNECTING is
 * "no address yet", CONNECTED is "has one", RETRYING is "lost the link".
 */
#include "netmgr.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "netmgr-eth";

#define NVS_NS        "upsa"
#define NVS_KEY_IPCFG "ipcfg"         /* netmgr_ipcfg_t, as a blob */
#define GW_PINGS      3

/* Same bound as the Wi-Fi build: after this long with no address, reboot and
 * start clean. A PHY that has stopped negotiating looks exactly like a dead
 * board to the person who needs it. */
#define OFFLINE_REBOOT_US  ((int64_t)15 * 60 * 1000000)
#define WATCHDOG_PERIOD_US ((int64_t)60 * 1000000)

static netmgr_state_t s_state = NETMGR_BOOTING;
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static char s_host[24] = "ups-esp32";
static char s_ip[16] = "0.0.0.0";
static bool s_mdns_up;
static int64_t s_last_online_us;
static esp_timer_handle_t s_offline_timer, s_restart_timer;
static netmgr_ipcfg_t s_ipcfg;          /* stored; is_static false = DHCP */
static bool s_static_failed;            /* this session fell back to DHCP */
static void (*s_gw_done)(bool ok);

static void offline_watchdog_cb(void *arg)
{
    if (s_state == NETMGR_STA_CONNECTED) return;
    int64_t off = esp_timer_get_time() - s_last_online_us;
    if (off < OFFLINE_REBOOT_US) {
        ESP_LOGW(TAG, "off the network for %llds", (long long)(off / 1000000));
        return;
    }
    ESP_LOGE(TAG, "off the network for %llds, restarting", (long long)(off / 1000000));
    esp_restart();
}

static void mdns_bring_up(void)
{
    if (s_mdns_up) return;
    /* A failure here must not stop the device serving NUT. */
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed, continuing without .local name");
        return;
    }
    mdns_hostname_set(s_host);
    mdns_instance_name_set(s_host);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_nut",  "_tcp", 3493, NULL, 0);
    s_mdns_up = true;
    ESP_LOGI(TAG, "mdns up: %s.local", s_host);
}

static void restart_cb(void *arg) { esp_restart(); }

static void restart_in(int64_t us)
{
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "eth_restart" };
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_restart_timer));
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, us);
}

/* ---- fixed address -------------------------------------------------------- */

static bool parse4(const char *s, esp_ip4_addr_t *out)
{
    return s && s[0] && esp_netif_str_to_ip4(s, out) == ESP_OK;
}

static esp_err_t store_ipcfg(const netmgr_ipcfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (c && c->is_static) nvs_set_blob(h, NVS_KEY_IPCFG, c, sizeof(*c));
    else                   nvs_erase_key(h, NVS_KEY_IPCFG);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool netmgr_ipcfg_check(const netmgr_ipcfg_t *c, char *why, size_t n)
{
    if (!c->is_static) return true;
    esp_ip4_addr_t ip, mask, gw, dns;
    if (!parse4(c->ip, &ip))     { strlcpy(why, "the address is not a valid IPv4 address", n); return false; }
    if (!parse4(c->mask, &mask)) { strlcpy(why, "the subnet mask is not valid", n); return false; }
    if (!parse4(c->gw, &gw))     { strlcpy(why, "the gateway is not a valid IPv4 address", n); return false; }
    if (c->dns[0] && !parse4(c->dns, &dns)) { strlcpy(why, "the DNS server is not a valid IPv4 address", n); return false; }
    uint32_t m = ntohl(mask.addr), a = ntohl(ip.addr), g = ntohl(gw.addr);
    if (m == 0 || (m | (m - 1)) != 0xFFFFFFFFu || m < 0xFF000000u || m > 0xFFFFFFFCu) {
        strlcpy(why, "the subnet mask is not valid (e.g. 255.255.255.0)", n); return false;
    }
    if ((a & m) != (g & m)) { strlcpy(why, "the gateway is not on the same subnet as the address", n); return false; }
    if ((a & ~m) == 0 || (a & ~m) == ~m) {
        strlcpy(why, "that is the subnet's network or broadcast address", n); return false;
    }
    if (a == g) { strlcpy(why, "the address and the gateway are the same", n); return false; }
    uint8_t first = a >> 24;
    if (first == 0 || first == 127 || first >= 224) { strlcpy(why, "that address cannot be used on a LAN", n); return false; }
    return true;
}

static void gw_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_delete_session(hdl);
    void (*done)(bool) = s_gw_done;
    s_gw_done = NULL;
    if (done) done(received > 0);
}

/* Three pings to the gateway, from the ping task; done() runs there. */
static void gw_check_start(const char *gw, void (*done)(bool ok))
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target = {0};
    esp_ip4_addr_t g;
    if (!parse4(gw, &g)) { done(false); return; }
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = g.addr;
    cfg.target_addr = target;
    cfg.count = GW_PINGS;
    cfg.interval_ms = 700;
    cfg.timeout_ms = 1500;
    cfg.task_stack_size = 2560;
    esp_ping_callbacks_t cbs = { .on_ping_end = gw_ping_end };
    esp_ping_handle_t h;
    s_gw_done = done;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK || esp_ping_start(h) != ESP_OK) {
        s_gw_done = NULL;
        done(false);
    }
}

static void static_checked(bool ok)
{
    if (ok) { ESP_LOGI(TAG, "gateway answers from the fixed address"); return; }
    ESP_LOGE(TAG, "gateway %s did not answer from %s; using DHCP until restart",
             s_ipcfg.gw, s_ipcfg.ip);
    s_static_failed = true;
    esp_netif_dhcpc_start(s_netif);     /* GOT_IP follows from DHCP */
}

/* Put the fixed address on the interface; GOT_IP is posted as if DHCP had
 * answered. Done before the link comes up, so DHCP never starts. */
static void apply_static(const netmgr_ipcfg_t *c)
{
    esp_netif_ip_info_t info = {0};
    parse4(c->ip, &info.ip);
    parse4(c->mask, &info.netmask);
    parse4(c->gw, &info.gw);
    esp_netif_dhcpc_stop(s_netif);
    if (esp_netif_set_ip_info(s_netif, &info) != ESP_OK) {
        ESP_LOGE(TAG, "could not set the fixed address; using DHCP");
        esp_netif_dhcpc_start(s_netif);
        return;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (!parse4(c->dns[0] ? c->dns : c->gw, &dns.ip.u_addr.ip4)) parse4(c->gw, &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "fixed address %s/%s via %s", c->ip, c->mask, c->gw);
}

/* ---- events ---------------------------------------------------------------- */

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        if (s_state != NETMGR_STA_CONNECTED) s_state = NETMGR_STA_CONNECTING;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        /* The driver renegotiates by itself when the cable returns; nothing
         * to retry from here. */
        ESP_LOGW(TAG, "link down");
        s_state = NETMGR_STA_RETRYING;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "ethernet started");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_LOST_IP) {
        s_state = NETMGR_STA_RETRYING;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        return;
    }
    const ip_event_got_ip_t *ev = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_state = NETMGR_STA_CONNECTED;
    s_last_online_us = esp_timer_get_time();
    ESP_LOGI(TAG, "got address %s", s_ip);
    mdns_bring_up();
    /* Prove a fixed address once per session, on the first time it is used. */
    static bool checked;
    if (s_ipcfg.is_static && !s_static_failed && !checked) {
        checked = true;
        gw_check_start(s_ipcfg.gw, static_checked);
    }
}

/* ---- start ----------------------------------------------------------------- */

esp_err_t netmgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_last_online_us = esp_timer_get_time();
    const esp_timer_create_args_t off_args = { .callback = offline_watchdog_cb, .name = "eth_offline" };
    ESP_ERROR_CHECK(esp_timer_create(&off_args, &s_offline_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_offline_timer, WATCHDOG_PERIOD_US));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(s_host, sizeof(s_host), "ups-esp32-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL));

    nvs_handle_t ih;
    if (nvs_open(NVS_NS, NVS_READONLY, &ih) == ESP_OK) {
        size_t len = sizeof(s_ipcfg);
        if (nvs_get_blob(ih, NVS_KEY_IPCFG, &s_ipcfg, &len) != ESP_OK || len != sizeof(s_ipcfg)) {
            memset(&s_ipcfg, 0, sizeof(s_ipcfg));
        }
        nvs_close(ih);
    }
    char why[80];
    if (s_ipcfg.is_static && !netmgr_ipcfg_check(&s_ipcfg, why, sizeof(why))) {
        ESP_LOGE(TAG, "stored fixed address ignored: %s", why);
        s_ipcfg.is_static = false;
    }

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    esp_netif_set_hostname(s_netif, s_host);

    /* IP101 on RMII; the PHY supplies the 50 MHz reference clock, which comes
     * in on GPIO50. Pins are the board's (board.h), never numbers here. */
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = BOARD_ETH_PIN_MDC;
    emac.smi_gpio.mdio_num = BOARD_ETH_PIN_MDIO;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = BOARD_ETH_PIN_REFCLK;
    eth_mac_config_t mcfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac_dev = esp_eth_mac_new_esp32(&emac, &mcfg);
    eth_phy_config_t pcfg = ETH_PHY_DEFAULT_CONFIG();
    pcfg.phy_addr = BOARD_ETH_PHY_ADDR;
    pcfg.reset_gpio_num = BOARD_ETH_PIN_RESET;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&pcfg);
    if (!mac_dev || !phy) {
        ESP_LOGE(TAG, "could not create the Ethernet MAC/PHY");
        return ESP_FAIL;
    }
    esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac_dev, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&ecfg, &s_eth));
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));

    if (s_ipcfg.is_static) apply_static(&s_ipcfg);
    s_state = NETMGR_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
    ESP_LOGI(TAG, "Ethernet starting as %s", s_host);
    return ESP_OK;
}

/* ---- accessors ------------------------------------------------------------- */

netmgr_state_t netmgr_state(void) { return s_state; }
const char *netmgr_ip(void) { return s_ip; }
const char *netmgr_hostname(void) { return s_host; }

/* Nothing here is wireless. The page already hides what is empty. */
const char *netmgr_ap_ssid(void) { return ""; }
const char *netmgr_ssid(void) { return ""; }
int netmgr_rssi(void) { return 0; }
int netmgr_scan(netmgr_ap_t *out, int max) { return -1; }

netmgr_join_t netmgr_join_state(void) { return NETMGR_JOIN_IDLE; }
const char *netmgr_join_ip(void) { return ""; }
const char *netmgr_join_error(void) { return "this board has no Wi-Fi; it uses its Ethernet cable"; }

esp_err_t netmgr_provision(const char *ssid, const char *pass, const netmgr_ipcfg_t *ip)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void netmgr_ipcfg_get(netmgr_ipcfg_t *out, bool *fell_back)
{
    *out = s_ipcfg;
    if (fell_back) *fell_back = s_static_failed;
}

void netmgr_ipcfg_current(netmgr_ipcfg_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_netif_ip_info_t info;
    if (s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
        esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
        esp_netif_dhcpc_get_status(s_netif, &st);
        out->is_static = st != ESP_NETIF_DHCP_STARTED;
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&info.ip));
        snprintf(out->mask, sizeof(out->mask), IPSTR, IP2STR(&info.netmask));
        snprintf(out->gw, sizeof(out->gw), IPSTR, IP2STR(&info.gw));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            snprintf(out->dns, sizeof(out->dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
}

esp_err_t netmgr_ipcfg_set(const netmgr_ipcfg_t *c, char *why, size_t n)
{
    if (!netmgr_ipcfg_check(c, why, n)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = store_ipcfg(c);
    if (err != ESP_OK) { strlcpy(why, "could not save the settings", n); return err; }
    ESP_LOGI(TAG, "address set to %s, restarting onto it", c->is_static ? c->ip : "DHCP");
    restart_in(1200 * 1000);
    return ESP_OK;
}

esp_err_t netmgr_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
    return ESP_OK;
}
