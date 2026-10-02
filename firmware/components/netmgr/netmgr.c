/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netmgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "netmgr";

#define NVS_NS        "upsa"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"
#define NVS_KEY_IPCFG "ipcfg"         /* netmgr_ipcfg_t, as a blob */

/* A fixed address must prove itself: the gateway has to answer a ping from
 * it. If not -- a typo, a moved router, an address the network does not
 * route -- the board falls back to DHCP for the rest of that session rather
 * than sit on the network unreachable, and the page says so. */
#define GW_PINGS          3

/* Backoff is capped rather than exponential-forever: a UPS bridge that has
 * been offline for an hour must still rejoin within 30s of the AP returning,
 * not sleep for another hour. */
#define RETRY_MIN_MS  1000
#define RETRY_MAX_MS  30000

/* How long this device may stay off the network before it restarts itself.
 *
 * The retry loop below never gives up, and that is still not enough: retrying
 * forever only helps if the Wi-Fi stack is in a state where connecting can
 * succeed. A board that is running, polling its UPS and answering nothing is
 * indistinguishable from a dead one to the person who needs it, and there is
 * no cable in a closet. This bounds the damage -- after this long with no
 * address, reboot and start clean.
 *
 * Fifteen minutes is chosen to be far longer than any normal outage: a router
 * rebooting, an AP roaming, a brief power cut to the access point. If it fires
 * during one of those, the cost is a thirty-second restart nobody notices.
 * What it prevents is the two-hour silence that prompted it. */
#define OFFLINE_REBOOT_US  ((int64_t)15 * 60 * 1000000)
#define WATCHDOG_PERIOD_US ((int64_t)60 * 1000000)

/* How long a setup join may take before the page is told it failed. Long
 * enough for a slow DHCP server; short enough that a person holding a phone
 * is not left wondering. */
#define JOIN_TIMEOUT_US    ((int64_t)25 * 1000000)
/* After a setup join succeeds the AP stays up this long, so the person can
 * read the address off the page, then the board restarts as a plain station.
 * The page's Done button restarts sooner, through /api/reboot. */
#define JOIN_LINGER_US     ((int64_t)180 * 1000000)
/* Reasons that mean "wrong password" rather than "try again", and how many to
 * see before saying so: a single handshake timeout can be radio noise. */
#define JOIN_AUTH_STRIKES  2

/* Rescue: a board whose stored network will not have it, opened up again.
 *
 * Setup used to start only with no credentials stored at all. A board whose
 * password was mistyped from its own page, or whose router changed name or
 * password, retried the old network for ever and restarted every fifteen
 * minutes into the same thing, out of reach of everyone until someone pressed
 * BOOT five times by hand. Now, after RESCUE_AFTER offline restarts in a row
 * (half an hour without a network), it comes up in setup for RESCUE_WINDOW_US
 * instead, and then tries the stored network again: half an hour on the
 * stored network, five minutes of setup, and so on, until one of them works.
 * The window stays open while anyone is connected to it.
 *
 * Changing the network from that setup page still needs the board's login
 * when one exists (webui may_write), so an open setup network on a board that
 * has an owner hands a stranger nothing.
 *
 * Counted in RTC memory, which a restart keeps and a power cut clears: a
 * board that has just been powered on always tries its network first. */
#define RESCUE_AFTER       2
#define RESCUE_WINDOW_US   ((int64_t)5 * 60 * 1000000)
#define RESCUE_MAGIC       0x52455343u     /* "RESC" */
static RTC_NOINIT_ATTR struct { uint32_t magic; uint32_t offline_restarts; } s_rtc;
static bool s_rescue;                       /* in setup because of the above */
static esp_timer_handle_t s_rescue_timer;
static int64_t s_rescue_until_us;

static netmgr_state_t s_state = NETMGR_BOOTING;
/* ups-esp32-XXXX. Advertised over mDNS as <this>.local -- how the web UI is
 * found without a DHCP table -- and Home Assistant discovers the NUT service
 * from the same record. Per board, because two boards both claiming
 * ups-esp32.local means a customer's bookmark reaches whichever answered
 * first. */
static char s_host[24] = "ups-esp32";
static esp_netif_t *s_sta_netif;
static netmgr_join_t s_join = NETMGR_JOIN_IDLE;
static char s_join_ip[16] = "";
static char s_join_err[96] = "";
static char s_join_pass[65];
static int s_join_auth_fails;
static int s_join_missing;
static esp_timer_handle_t s_join_timer;
static bool s_mdns_up = false;
static esp_timer_handle_t s_retry_timer;
static char s_ip[16] = "0.0.0.0";
static char s_ap_ssid[33] = "";
static char s_ssid[33] = "";          /* the network we are on, or joining */
static uint32_t s_retry_ms = RETRY_MIN_MS;
static esp_timer_handle_t s_restart_timer;
static esp_timer_handle_t s_offline_timer;
/* When this device last held an IP address, or booted. Compared against
 * OFFLINE_REBOOT_US by offline_watchdog_cb. */
static int64_t s_last_online_us;

static netmgr_ipcfg_t s_ipcfg;          /* stored; is_static false = DHCP */
static netmgr_ipcfg_t s_join_ipcfg;     /* for a setup join in progress */
static bool s_static_failed;            /* this session fell back to DHCP */
static void gw_check_start(const char *gw, void (*done)(bool ok));

/* Reconnect from the timer's own task, never from the event handler.
 *
 * Wi-Fi and IP events are delivered one at a time by a single default event
 * loop task. Sleeping inside a handler stalls every other event behind it, so
 * the backoff waits here instead and the handler returns immediately. */
static void retry_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "reconnecting");
    /* A connect refused outright (a page scan running, say) posts no
     * DISCONNECTED event, and nothing would arm this timer again: the board
     * sat offline until the fifteen-minute restart. Try again instead. */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect refused (%s), trying again shortly", esp_err_to_name(err));
        esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_MAX_MS * 1000);
    }
}

/* Periodic: has this device been unreachable for too long?
 *
 * Deliberately not armed while provisioning -- a board sitting in its setup AP
 * is waiting for a person, has no credentials to connect with, and rebooting
 * out from under someone typing their password would be its own bug. */
static void offline_watchdog_cb(void *arg)
{
    if (s_state == NETMGR_AP_PROVISIONING) {
        s_last_online_us = esp_timer_get_time();
        return;
    }
    if (s_state == NETMGR_STA_CONNECTED) return;

    int64_t off = esp_timer_get_time() - s_last_online_us;
    if (off < OFFLINE_REBOOT_US) {
        ESP_LOGW(TAG, "off the network for %llds", (long long)(off / 1000000));
        return;
    }
    ESP_LOGE(TAG, "off the network for %llds, restarting",
             (long long)(off / 1000000));
    if (s_rtc.magic != RESCUE_MAGIC) { s_rtc.magic = RESCUE_MAGIC; s_rtc.offline_restarts = 0; }
    s_rtc.offline_restarts++;
    esp_restart();
}

/* Every 30 s while in rescue setup: close the window, unless someone is on it
 * or a join is under way, and go back to trying the stored network. */
static void rescue_timer_cb(void *arg)
{
    wifi_sta_list_t sta;
    bool someone = esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0;
    if (someone || s_join == NETMGR_JOIN_TRYING || s_join == NETMGR_JOIN_OK) {
        s_rescue_until_us = esp_timer_get_time() + RESCUE_WINDOW_US;
        return;
    }
    if (esp_timer_get_time() < s_rescue_until_us) return;
    ESP_LOGW(TAG, "nobody used the setup network; trying \"%s\" again", s_ssid);
    /* One short of RESCUE_AFTER: back here after one more failed stretch,
     * not two. */
    s_rtc.magic = RESCUE_MAGIC;
    s_rtc.offline_restarts = RESCUE_AFTER - 1;
    esp_restart();
}

static void mdns_bring_up(void)
{
    if (s_mdns_up) return;

    /* A failure here must not stop the device serving NUT: mDNS is a
     * convenience, and the IP still works. */
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
        const esp_timer_create_args_t a = { .callback = restart_cb,
                                            .name = "provision_restart" };
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_restart_timer));
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, us);
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

static bool parse4(const char *s, esp_ip4_addr_t *out)
{
    return s && s[0] && esp_netif_str_to_ip4(s, out) == ESP_OK;
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
    /* Contiguous ones, then zeros; /8 to /30 */
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

/* Put the fixed address on the station interface. Called on association:
 * esp_netif then posts IP_EVENT_STA_GOT_IP as if DHCP had answered. */
static void apply_static(const netmgr_ipcfg_t *c)
{
    esp_netif_ip_info_t info = {0};
    parse4(c->ip, &info.ip);
    parse4(c->mask, &info.netmask);
    parse4(c->gw, &info.gw);
    esp_netif_dhcpc_stop(s_sta_netif);
    if (esp_netif_set_ip_info(s_sta_netif, &info) != ESP_OK) {
        ESP_LOGE(TAG, "could not set the fixed address; using DHCP");
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (!parse4(c->dns[0] ? c->dns : c->gw, &dns.ip.u_addr.ip4)) parse4(c->gw, &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "fixed address %s/%s via %s", c->ip, c->mask, c->gw);
}

static void static_checked(bool ok)
{
    if (ok) { ESP_LOGI(TAG, "gateway answers from the fixed address"); return; }
    ESP_LOGE(TAG, "gateway %s did not answer from %s; using DHCP until restart",
             s_ipcfg.gw, s_ipcfg.ip);
    s_static_failed = true;
    esp_netif_dhcpc_start(s_sta_netif);     /* GOT_IP follows from DHCP */
}

static esp_err_t store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* End a setup join that did not work. The AP is still up and the page is
 * still polling, so this only has to say why and stop the station trying. */
static void join_fail(const char *why)
{
    if (s_join != NETMGR_JOIN_TRYING) return;
    esp_timer_stop(s_join_timer);
    esp_timer_stop(s_retry_timer);
    strlcpy(s_join_err, why, sizeof(s_join_err));
    s_join = NETMGR_JOIN_FAILED;
    esp_wifi_disconnect();      /* its DISCONNECTED event is ignored: not TRYING */
    ESP_LOGW(TAG, "setup join to \"%s\" failed: %s", s_ssid, why);
}

static void join_timeout_cb(void *arg)
{
    join_fail(s_join_auth_fails ? "the password was not accepted"
            : s_join_missing   ? "that network is not in range"
                               : "no address from the network in 25 seconds");
}

/* Wi-Fi and IP events while the setup AP is up. Only a join the person asked
 * for is acted on: without one the STA side exists just so the page can scan,
 * and chasing an association with no credentials would fill the log. */
static void join_checked(bool ok)
{
    if (s_join != NETMGR_JOIN_TRYING) return;
    if (!ok) {
        char why[96];
        snprintf(why, sizeof(why), "joined, but the gateway %s did not answer from %s",
                 s_join_ipcfg.gw, s_join_ipcfg.ip);
        join_fail(why);
        return;
    }
    if (store_credentials(s_ssid, s_join_pass) != ESP_OK || store_ipcfg(&s_join_ipcfg) != ESP_OK) {
        join_fail("joined, but could not save the settings");
        return;
    }
    strlcpy(s_join_ip, s_join_ipcfg.ip, sizeof(s_join_ip));
    s_join = NETMGR_JOIN_OK;
    mdns_bring_up();
    restart_in(JOIN_LINGER_US);
    ESP_LOGI(TAG, "setup join: \"%s\" on fixed %s, restarting as a station in %llds",
             s_ssid, s_join_ip, (long long)(JOIN_LINGER_US / 1000000));
}

static void on_setup_event(esp_event_base_t base, int32_t id, void *data)
{
    if (s_join != NETMGR_JOIN_TRYING) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (s_join_ipcfg.is_static) apply_static(&s_join_ipcfg);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t r = ((wifi_event_sta_disconnected_t *)data)->reason;
        bool auth = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                 || r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE;
        ESP_LOGI(TAG, "setup join: disconnected, reason %u", r);
        if (auth && ++s_join_auth_fails >= JOIN_AUTH_STRIKES) {
            join_fail("the password was not accepted");
            return;
        }
        if (r == WIFI_REASON_NO_AP_FOUND && ++s_join_missing >= 3) {
            join_fail("that network is not in range");
            return;
        }
        /* From the timer, never from here: see retry_timer_cb. */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, RETRY_MIN_MS * 1000);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_join_ipcfg.is_static) {
        /* A fixed address is not proof of anything: the gateway must answer. */
        esp_timer_stop(s_join_timer);
        gw_check_start(s_join_ipcfg.gw, join_checked);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_timer_stop(s_join_timer);
        /* Stored only now. A typo never reaches flash, so it can never become
         * a board that reboots into a network it cannot join. */
        if (store_credentials(s_ssid, s_join_pass) != ESP_OK || store_ipcfg(NULL) != ESP_OK) {
            join_fail("joined, but could not save the settings");
            return;
        }
        snprintf(s_join_ip, sizeof(s_join_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_join = NETMGR_JOIN_OK;
        mdns_bring_up();       /* so <host>.local answers before the restart */
        restart_in(JOIN_LINGER_US);
        ESP_LOGI(TAG, "setup join: \"%s\" gave %s, restarting as a station in %llds",
                 s_ssid, s_join_ip, (long long)(JOIN_LINGER_US / 1000000));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (s_state == NETMGR_AP_PROVISIONING) {
        on_setup_event(base, id, data);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_state = NETMGR_STA_CONNECTING;
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (s_ipcfg.is_static && !s_static_failed) apply_static(&s_ipcfg);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The bug this component exists to avoid: never give up. */
        s_state = NETMGR_STA_RETRYING;
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        ESP_LOGW(TAG, "disconnected, retrying in %lums", (unsigned long)s_retry_ms);

        /* Arm and return. Restarting an already-armed timer is harmless and
         * collapses a burst of disconnect events into one pending attempt. */
        esp_timer_stop(s_retry_timer);
        /* NOT ESP_ERROR_CHECK. This is the code whose entire job is to never
         * give up; aborting here would reboot the device on a transient timer
         * error, and an abort in a retry path is how a recoverable failure
         * becomes a boot loop. If arming fails, say so and let the offline
         * watchdog be the backstop. */
        esp_err_t terr = esp_timer_start_once(s_retry_timer,
                                              (uint64_t)s_retry_ms * 1000);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "could not arm the reconnect timer: %s",
                     esp_err_to_name(terr));
            esp_wifi_connect();      /* try immediately rather than never */
        }
        s_retry_ms = (s_retry_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2;

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        esp_timer_stop(s_retry_timer);      /* cancel any pending attempt */
        s_retry_ms = RETRY_MIN_MS;          /* reset backoff on success */
        s_last_online_us = esp_timer_get_time();
        s_rtc.magic = RESCUE_MAGIC;
        s_rtc.offline_restarts = 0;
        s_state = NETMGR_STA_CONNECTED;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
        mdns_bring_up();   /* idempotent: survives reconnects */
        if (s_ipcfg.is_static && !s_static_failed) gw_check_start(s_ipcfg.gw, static_checked);
    }
}

static bool load_credentials(char *ssid, size_t ssid_len,
                             char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

/* Answer every DNS question with our own address.
 *
 * This is what makes a phone pop the setup page by itself: iOS and Android
 * fetch a known URL after joining a network, and if the reply is not what they
 * expect they show the page in a captive-portal sheet. That only works if the
 * lookup resolves, and on an isolated AP there is nothing to resolve it.
 *
 * The reply is built by hand rather than with a DNS library: a question is
 * echoed back verbatim with one answer appended, which is a dozen lines, and
 * this socket only ever has to satisfy a captive-portal probe.
 */
static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "captive DNS socket failed, setup page needs a typed URL");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "captive DNS bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t pkt[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from, &flen);
        /* 12-byte header, one question, and room for a 16-byte answer. */
        if (n < 12 + 5 || n + 16 > (int)sizeof(pkt)) continue;
        if (pkt[2] & 0x80) continue;              /* already a response */
        if (ntohs(*(uint16_t *)&pkt[4]) != 1) continue;   /* exactly one question */

        pkt[2] = 0x84;                  /* response, authoritative */
        pkt[3] = 0x00;                  /* no error */
        pkt[7] = 0x01;                  /* ANCOUNT = 1 */

        uint8_t *a = pkt + n;
        *a++ = 0xC0; *a++ = 0x0C;       /* name: pointer to the question */
        *a++ = 0x00; *a++ = 0x01;       /* type A */
        *a++ = 0x00; *a++ = 0x01;       /* class IN */
        *a++ = 0; *a++ = 0; *a++ = 0; *a++ = 60;          /* TTL 60s */
        *a++ = 0x00; *a++ = 0x04;       /* RDLENGTH 4 */
        *a++ = 192; *a++ = 168; *a++ = 4; *a++ = 1;       /* 192.168.4.1 */

        sendto(sock, pkt, a - pkt, 0, (struct sockaddr *)&from, flen);
    }
}

/* Open network, on purpose. A WPA2 passphrase on a setup AP has to be printed
 * on the board or the manual, which means it is public anyway, and it stops a
 * phone joining automatically. Nothing here is worth protecting for the couple
 * of minutes it is up: the only thing the AP accepts is the credentials for
 * the network the person is standing in. */
static void start_softap(void)
{
    esp_netif_create_default_wifi_ap();
    /* APSTA: the setup page scans with it, and joins with it in place. */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, s_host);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    strlcpy(s_ap_ssid, s_host, sizeof(s_ap_ssid));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(s_ap_ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    strlcpy(s_ip, "192.168.4.1", sizeof(s_ip));
    xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "provisioning AP \"%s\" up -- join it and open http://192.168.4.1/",
             s_ap_ssid);
}

esp_err_t netmgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_last_online_us = esp_timer_get_time();
    const esp_timer_create_args_t off_args = {
        .callback = offline_watchdog_cb,
        .name = "wifi_offline",
    };
    ESP_ERROR_CHECK(esp_timer_create(&off_args, &s_offline_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_offline_timer, WATCHDOG_PERIOD_US));

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    const esp_timer_create_args_t join_args = {
        .callback = join_timeout_cb,
        .name = "wifi_join",
    };
    ESP_ERROR_CHECK(esp_timer_create(&join_args, &s_join_timer));

    /* The SoftAP MAC, in both modes, so the name a board had during setup is
     * the name it keeps afterwards. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_host, sizeof(s_host), "ups-esp32-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

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

    char ssid[33] = {0}, pass[65] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "no credentials stored, starting provisioning AP");
        s_state = NETMGR_AP_PROVISIONING;
        start_softap();
        return ESP_OK;
    }
    if (esp_reset_reason() == ESP_RST_POWERON || s_rtc.magic != RESCUE_MAGIC) {
        s_rtc.magic = RESCUE_MAGIC;
        s_rtc.offline_restarts = 0;
    }
    if (s_rtc.offline_restarts >= RESCUE_AFTER) {
        ESP_LOGW(TAG, "could not join \"%s\" for %u restarts: setup network up for "
                      "%llds", ssid, (unsigned)s_rtc.offline_restarts,
                 (long long)(RESCUE_WINDOW_US / 1000000));
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        s_rescue = true;
        s_state = NETMGR_AP_PROVISIONING;
        start_softap();
        const esp_timer_create_args_t ra = { .callback = rescue_timer_cb,
                                             .name = "wifi_rescue" };
        s_rescue_until_us = esp_timer_get_time() + RESCUE_WINDOW_US;
        if (esp_timer_create(&ra, &s_rescue_timer) == ESP_OK) {
            esp_timer_start_periodic(s_rescue_timer, 30 * 1000000);
        }
        return ESP_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    /* The name in the router's client list, for anyone who does go looking. */
    esp_netif_set_hostname(s_sta_netif, s_host);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* Never sleep the radio: a NUT client may connect at any moment, and the
     * power saving is irrelevant on a mains-powered bridge. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

netmgr_state_t netmgr_state(void) { return s_state; }
bool netmgr_rescue(void) { return s_rescue; }
const char *netmgr_ip(void) { return s_ip; }

const char *netmgr_ap_ssid(void) { return s_ap_ssid; }
const char *netmgr_ssid(void) { return s_ssid; }

/* Signal strength of the AP we are associated with, in dBm. 0 when we are not
 * associated -- a real reading is always negative, so the caller can tell. */
int netmgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state != NETMGR_STA_CONNECTED) return 0;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

netmgr_join_t netmgr_join_state(void) { return s_join; }
const char *netmgr_join_ip(void) { return s_join_ip; }
const char *netmgr_join_error(void) { return s_join_err; }
const char *netmgr_hostname(void) { return s_host; }

/* From the setup AP, join in place and report; see netmgr.h for why.
 *
 * The AP is NOT switched off here. It goes by restarting, once the address
 * has been shown: the live APSTA->STA switch would tear down the AP the
 * browser is talking to, and a reboot does the same in a known order. What
 * changed is only that the restart now waits until the person has what they
 * need to find the board again.
 *
 * Associating moves the AP onto the router's channel, so a phone on it may
 * drop for a few seconds and rejoin. The page keeps polling through that. */
esp_err_t netmgr_provision(const char *ssid, const char *pass, const netmgr_ipcfg_t *ip)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";
    char why[80];
    if (ip && !netmgr_ipcfg_check(ip, why, sizeof(why))) return ESP_ERR_INVALID_ARG;
    netmgr_ipcfg_t cfg = {0};
    if (ip) cfg = *ip;

    if (s_state == NETMGR_AP_PROVISIONING) {
        if (s_join == NETMGR_JOIN_TRYING) return ESP_ERR_INVALID_STATE;

        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        /* No disconnect first: in setup the station is never associated
         * except during a join, and a failed join has already let go. A
         * disconnect here would post an event that lands mid-attempt. */
        esp_timer_stop(s_retry_timer);
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) return err;

        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_join_pass, pass, sizeof(s_join_pass));
        s_join_ipcfg = cfg;
        if (!cfg.is_static) esp_netif_dhcpc_start(s_sta_netif);   /* after a static try */
        s_join_err[0] = '\0';
        s_join_ip[0] = '\0';
        s_join_auth_fails = 0;
        s_join_missing = 0;
        s_join = NETMGR_JOIN_TRYING;
        esp_timer_stop(s_join_timer);
        esp_timer_start_once(s_join_timer, JOIN_TIMEOUT_US);
        ESP_LOGI(TAG, "setup join: trying \"%s\" with the setup AP still up", ssid);
        esp_wifi_connect();
        return ESP_OK;
    }

    /* Already on a network and asked to move. The page is reached over the
     * network being left, so there is nothing to report to: store, restart,
     * and let rescue (RESCUE_AFTER, above) catch a mistake: half an hour on,
     * the setup network comes up to put it right. The delay lets the HTTP
     * response leave first. */
    esp_err_t err = store_credentials(ssid, pass);
    if (err == ESP_OK) err = store_ipcfg(&cfg);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "credentials stored for \"%s\", restarting to connect", ssid);
    restart_in(1200 * 1000);
    return ESP_OK;
}

/* A blocking scan, called from the web server's task. It takes a couple of
 * seconds and stalls that one request, which is the right trade for a page
 * whose whole job at that moment is to list networks. */
int netmgr_scan(netmgr_ap_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) return 0;

    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (!recs) return -1;
    esp_wifi_scan_get_ap_records(&found, recs);

    /* IDF returns these strongest-first. Collapse duplicate SSIDs -- a mesh
     * shows one per radio, and the list is for a human to pick from. */
    int n = 0;
    for (int i = 0; i < found && n < max; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid));
        out[n].rssi = recs[i].rssi;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(recs);
    return n;
}

/* ---- fixed address: gateway check and accessors ---------------------------- */
#include "ping/ping_sock.h"

static void (*s_gw_done)(bool ok);

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

void netmgr_ipcfg_get(netmgr_ipcfg_t *out, bool *fell_back)
{
    *out = s_ipcfg;
    if (fell_back) *fell_back = s_static_failed;
}

void netmgr_ipcfg_current(netmgr_ipcfg_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_netif_ip_info_t info;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) {
        esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
        esp_netif_dhcpc_get_status(s_sta_netif, &st);
        out->is_static = st != ESP_NETIF_DHCP_STARTED;
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&info.ip));
        snprintf(out->mask, sizeof(out->mask), IPSTR, IP2STR(&info.netmask));
        snprintf(out->gw, sizeof(out->gw), IPSTR, IP2STR(&info.gw));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
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
