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
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "netmgr";

#define NVS_NS        "upsa"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

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

static netmgr_state_t s_state = NETMGR_BOOTING;
/* ups-adaptor-XXXX. Advertised over mDNS as <this>.local -- how the web UI is
 * found without a DHCP table -- and Home Assistant discovers the NUT service
 * from the same record. Per board, because two boards both claiming
 * ups-adaptor.local means a customer's bookmark reaches whichever answered
 * first. */
static char s_host[24] = "ups-adaptor";
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

/* Reconnect from the timer's own task, never from the event handler.
 *
 * Wi-Fi and IP events are delivered one at a time by a single default event
 * loop task. Sleeping inside a handler stalls every other event behind it, so
 * the backoff waits here instead and the handler returns immediately. */
static void retry_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "reconnecting");
    esp_wifi_connect();
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
static void on_setup_event(esp_event_base_t base, int32_t id, void *data)
{
    if (s_join != NETMGR_JOIN_TRYING) return;

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

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_timer_stop(s_join_timer);
        /* Stored only now. A typo never reaches flash, so it can never become
         * a board that reboots into a network it cannot join. */
        if (store_credentials(s_ssid, s_join_pass) != ESP_OK) {
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
        s_state = NETMGR_STA_CONNECTED;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
        mdns_bring_up();   /* idempotent: survives reconnects */
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
    snprintf(s_host, sizeof(s_host), "ups-adaptor-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "no credentials stored, starting provisioning AP");
        s_state = NETMGR_AP_PROVISIONING;
        start_softap();
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
esp_err_t netmgr_provision(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";

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
     * and let the fallback to setup mode catch a mistake. The delay lets the
     * HTTP response leave first. */
    esp_err_t err = store_credentials(ssid, pass);
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
