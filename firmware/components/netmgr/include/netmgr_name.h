/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UPSA_NETMGR_NAME_H
#define UPSA_NETMGR_NAME_H

/* The checks for the names an owner can give the board and its UPS. Pure, so
 * tools/test/test_names.py compiles them on the host.
 *
 * The board name becomes the DHCP hostname, <name>.local, the HTTPS
 * certificate's names, the Tailscale device name and SNMP sysName: one DNS
 * label, lower case. The UPS name is NUT's (the "ups" in ups@host) and the
 * Prometheus ups label. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define NETMGR_NAME_MAX 32          /* characters, either name */

static inline bool netmgr_name_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* One DNS label: letters, digits and hyphens, 1 to 32 of them, no hyphen at
 * either end. Upper case is folded to lower in place (DNS is case blind, and
 * mDNS answers as given). Empty is not accepted here: the caller treats it as
 * "back to the default name". */
static inline bool netmgr_hostname_check(char *s, char *why, size_t n)
{
    size_t len = strlen(s);
    if (len == 0 || len > NETMGR_NAME_MAX) {
        if (why) snprintf(why, n, "the board name must be 1 to %d characters", NETMGR_NAME_MAX);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
        if (!netmgr_name_alnum(s[i]) && s[i] != '-') {
            if (why) snprintf(why, n, "the board name can use only letters, digits and -");
            return false;
        }
    }
    if (s[0] == '-' || s[len - 1] == '-') {
        if (why) snprintf(why, n, "the board name cannot start or end with -");
        return false;
    }
    return true;
}

/* A NUT UPS name: letters, digits, '-', '_' and '.', starting with a letter
 * or digit, 1 to 32 of them. Case is kept: NUT compares it exactly. Nothing
 * that NUT's protocol would split on (spaces, quotes) or that ups@host would
 * misread ('@', ':'). */
static inline bool netmgr_upsname_check(const char *s, char *why, size_t n)
{
    size_t len = strlen(s);
    if (len == 0 || len > NETMGR_NAME_MAX) {
        if (why) snprintf(why, n, "the UPS name must be 1 to %d characters", NETMGR_NAME_MAX);
        return false;
    }
    if (!netmgr_name_alnum(s[0])) {
        if (why) snprintf(why, n, "the UPS name must start with a letter or digit");
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!netmgr_name_alnum(s[i]) && s[i] != '-' && s[i] != '_' && s[i] != '.') {
            if (why) snprintf(why, n, "the UPS name can use only letters, digits, - _ and .");
            return false;
        }
    }
    return true;
}

#endif /* UPSA_NETMGR_NAME_H */
