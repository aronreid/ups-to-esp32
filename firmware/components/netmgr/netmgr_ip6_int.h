/* SPDX-License-Identifier: GPL-3.0-or-later
 * netmgr_ip6.c's hooks for netmgr.c and netmgr_eth.c only. */
#ifndef UPSA_NETMGR_IP6_INT_H
#define UPSA_NETMGR_IP6_INT_H

#include <stdbool.h>
#include "esp_netif.h"

/* Read the setting and hook DNS. After esp_netif_init(), before any netif. */
void netmgr_ip6_init(void);
/* Clear SLAAC from an interface's defaults unless it is the LAN one and the
 * setting is on. Every interface this component creates goes through it. */
void netmgr_ip6_flags(esp_netif_inherent_config_t *base, bool lan);
/* The LAN interface, once created. */
void netmgr_ip6_attach(esp_netif_t *netif);
/* On every IPv4 address (GOT_IP): link-local address and DHCPv6. */
void netmgr_ip6_link_up(void);
/* Raw addresses, for the mDNS alias. 0 with the setting off. */
int netmgr_ip6_raw(esp_ip6_addr_t *out, int max);

#endif
