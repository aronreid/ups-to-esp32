/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * IPv6 alongside IPv4: the "IPv4 only / IPv4 + IPv6" setting. Shared by the
 * Wi-Fi (netmgr.c) and Ethernet (netmgr_eth.c) builds.
 *
 * What it does, when on:
 *   - a link-local address on association, and SLAAC addresses from the
 *     router's adverts (CONFIG_LWIP_IPV6_AUTOCONFIG);
 *   - DNS servers from the adverts (RDNSS) or stateless DHCPv6, kept BEHIND
 *     the IPv4 one (dns_hook below);
 *   - NUT, the page and SNMP answer on the IPv6 addresses, but only to
 *     clients on this board's own network (netmgr_sockaddr_ok).
 * Off (the default), the interface gets no IPv6 address at all: the build
 * has the code, the setting decides whether any of it runs.
 *
 * No IPv6-only mode, on purpose: github.com and its release-asset hosts have
 * no AAAA records, so a board without IPv4 (and no NAT64) would silently stop
 * updating. No fixed IPv6 address either; SLAAC's EUI-64 address does not
 * change while the router's prefix does not. */
#include "netmgr.h"
#include "netmgr_ip6.h"
#include "netmgr_ip6_int.h"
#include "netmgr_ap.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/ip6_addr.h"
#if LWIP_IPV6_DHCP6
#include "lwip/dhcp6.h"
#endif
#include <string.h>
#include "sdkconfig.h"

static const char *TAG = "net6";

#define NVS_NS      "upsa"
#define NVS_KEY_IP6 "ip6"

static bool s_on;                    /* the stored setting, read once at boot */
static esp_netif_t *s_netif;         /* the LAN interface: STA or Ethernet */

/* ---- the setting ---------------------------------------------------------- */

bool netmgr_ip6_enabled(void) { return s_on; }

static void restart_cb(void *arg) { esp_restart(); }

esp_err_t netmgr_ip6_set(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* Off is the absence of the key, so a factory reset and a board that
     * never saw this setting read the same. */
    err = on ? nvs_set_u8(h, NVS_KEY_IP6, 1) : nvs_erase_key(h, NVS_KEY_IP6);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "%s saved, restarting onto it", on ? "IPv4 + IPv6" : "IPv4 only");
    /* After the reply has gone. */
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = restart_cb, .name = "ip6_restart" };
    if (!t && esp_timer_create(&a, &t) != ESP_OK) esp_restart();
    esp_timer_stop(t);
    esp_timer_start_once(t, 1200 * 1000);
    return ESP_OK;
}

/* ---- DNS: IPv4 first ------------------------------------------------------- *
 *
 * lwIP's router-advert (RDNSS) and stateless DHCPv6 code both write DNS slot
 * 0, the slot DHCPv4 fills, and every advert and lease renewal overwrites the
 * other. A board whose IPv6 resolver is broken would then fail its update
 * checks while IPv4 works. With CONFIG_LWIP_DNS_SETSERVER_WITH_NETIF every
 * one of those writes comes here instead: an IPv4 server goes where it was
 * asked, an IPv6 one goes to the last slot. lwIP tries the next slot when one
 * times out, so either family can carry a lookup the other cannot.
 * Runs in the TCP/IP task. */
#if LWIP_DNS_SETSERVER_WITH_NETIF
#define DNS_V6_SLOT (DNS_MAX_SERVERS - 1)

static void dns_hook(struct netif *netif, u8_t n, const ip_addr_t *srv)
{
    (void)netif;
    if (!srv) {                          /* a server withdrawn: its own slot */
        dns_setserver(n, NULL);
        return;
    }
    if (IP_IS_V6(srv)) {
        if (s_on && n == 0) dns_setserver(DNS_V6_SLOT, srv);   /* one IPv6 server is plenty */
        return;
    }
    if (s_on && n >= DNS_V6_SLOT) return;    /* that slot is IPv6's */
    dns_setserver(n, srv);
}
_Static_assert(DNS_MAX_SERVERS >= 2, "IPv6 DNS needs a slot after the IPv4 one");
#endif

/* ---- addresses ------------------------------------------------------------ */

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip6_t *e = data;
    if (!s_netif || e->esp_netif != s_netif) return;
    ESP_LOGI(TAG, "IPv6 address " IPV6STR, IPV62STR(e->ip6_info.ip));
    netmgr_names_mdns_alias_refresh();      /* the second .local name gets it too */
}

void netmgr_ip6_init(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_IP6, &v);
        nvs_close(h);
    }
    s_on = v != 0;
#if LWIP_DNS_SETSERVER_WITH_NETIF
    /* Once, and always: off, it passes IPv4 through as lwIP would. */
    if (dns_setserver_callback(dns_hook) != ERR_OK) ESP_LOGE(TAG, "DNS hook not set");
#endif
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL);
    ESP_LOGI(TAG, "%s", s_on ? "IPv4 + IPv6" : "IPv4 only");
}

void netmgr_ip6_flags(esp_netif_inherent_config_t *base, bool lan)
{
    /* The build turns SLAAC on for every interface's defaults; only the LAN
     * interface, with the setting on, keeps it. */
    if (!(lan && s_on)) base->flags &= ~ESP_NETIF_FLAG_IPV6_AUTOCONFIG_ENABLED;
}

void netmgr_ip6_attach(esp_netif_t *netif) { s_netif = netif; }

#if LWIP_IPV6_DHCP6
static esp_err_t dhcp6_on(void *ctx)
{
    struct netif *n = esp_netif_get_netif_impl((esp_netif_t *)ctx);
    return n && dhcp6_enable_stateless(n) == ERR_OK ? ESP_OK : ESP_FAIL;
}
#endif

void netmgr_ip6_link_up(void)
{
    if (!s_on || !s_netif) return;
    /* On every IPv4 address (netmgr calls this from GOT_IP, when the
     * interface is certainly up). A link-local address still held from
     * before is kept: making it again would restart duplicate detection.
     * The router's adverts then bring the SLAAC address (IP_EVENT_GOT_IP6). */
    esp_ip6_addr_t ll;
    if (esp_netif_get_ip6_linklocal(s_netif, &ll) != ESP_OK) {
        esp_err_t err = esp_netif_create_ip6_linklocal(s_netif);
        if (err != ESP_OK) ESP_LOGW(TAG, "no link-local address: %s", esp_err_to_name(err));
    }
#if LWIP_IPV6_DHCP6
    /* Stateless only: asks for DNS when an advert says "other config". */
    if (esp_netif_tcpip_exec(dhcp6_on, s_netif) != ESP_OK) ESP_LOGW(TAG, "DHCPv6 not started");
#endif
}

int netmgr_ip6_addrs(netmgr_ip6_addr_t *out, int max)
{
    if (!s_on || !s_netif || max <= 0) return 0;
    esp_ip6_addr_t a[LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(s_netif, a);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        ip6addr_ntoa_r((const ip6_addr_t *)&a[i], out[k].addr, sizeof(out[k].addr));
        /* lwIP writes hex in capitals; RFC 5952 and every browser use lower. */
        for (char *c = out[k].addr; *c; c++) if (*c >= 'A' && *c <= 'F') *c = (char)(*c - 'A' + 'a');
        esp_ip6_addr_type_t t = esp_netif_ip6_get_addr_type(&a[i]);
        out[k].scope = t == ESP_IP6_ADDR_IS_LINK_LOCAL   ? "link-local"
                     : t == ESP_IP6_ADDR_IS_UNIQUE_LOCAL ? "local"
                                                         : "global";
        k++;
    }
    return k;
}

int netmgr_ip6_raw(esp_ip6_addr_t *out, int max)
{
    if (!s_on || !s_netif || max <= 0) return 0;
    esp_ip6_addr_t a[LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(s_netif, a);
    if (n > max) n = max;
    memcpy(out, a, (size_t)n * sizeof(a[0]));
    return n;
}

/* ---- who may talk to us ----------------------------------------------------- */

bool netmgr_sockaddr_ok(const void *sa, unsigned len)
{
    const struct sockaddr *s = sa;
    if (!s || len < (socklen_t)sizeof(sa_family_t)) return false;
    if (s->sa_family == AF_INET) return true;
    if (s->sa_family != AF_INET6 || len < (socklen_t)sizeof(struct sockaddr_in6)) return false;
    const uint8_t *a = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
    esp_ip6_addr_t got[LWIP_IPV6_NUM_ADDRESSES];
    uint8_t mine[LWIP_IPV6_NUM_ADDRESSES][16];
    int n = s_on && s_netif ? esp_netif_get_all_ip6(s_netif, got) : 0;
    for (int i = 0; i < n; i++) memcpy(mine[i], got[i].addr, 16);
    return netmgr_ip6_peer_local(a, mine, n);
}

bool netmgr_fd_ok(int fd)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return false;
    return netmgr_sockaddr_ok(&ss, sl);
}

void netmgr_sockaddr_str(const void *sa, unsigned len, char *out, size_t cap)
{
    const struct sockaddr *s = sa;
    if (cap) out[0] = '\0';
    if (!s || len < (socklen_t)sizeof(sa_family_t)) return;
    uint32_t v4 = netmgr_sockaddr_v4((const struct sockaddr_storage *)sa, len);
    if (v4) {
        inet_ntop(AF_INET, &v4, out, cap);
    } else if (s->sa_family == AF_INET6 && len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr, out, cap);
    }
}
