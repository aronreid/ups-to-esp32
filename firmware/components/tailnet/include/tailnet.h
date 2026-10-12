/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Optional Tailscale client: joins this board to a tailnet, so NUT on 3493
 * and the web UI answer on a 100.x address from anywhere on it.
 *
 * Only built with CONFIG_UPSA_TAILSCALE (Rev B). Callers guard every call
 * with that symbol; with it off this header declares nothing that exists.
 *
 * The auth key and the on/off switch live in NVS, set from the web UI. They
 * are never in the image: one image is published for every Rev B board, and
 * a key baked into it would put every board on one person's tailnet.
 *
 * Follows netmgr rather than duplicating it. Tailscale starts when the station
 * gets an address, survives a Wi-Fi dropout by rebinding when the address
 * comes back, and rebuilds itself if it stays down -- it never restarts the
 * board. A UPS bridge that reboots because a VPN is unhappy would drop every
 * NUT client on the LAN for a feature most of them do not use.
 */
#ifndef UPSA_TAILNET_H
#define UPSA_TAILNET_H

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_UPSA_TAILSCALE

typedef enum {
    TAILNET_OFF,          /* switched off in settings */
    TAILNET_NO_KEY,       /* switched on, but no auth key stored */
    TAILNET_WAITING,      /* waiting for Wi-Fi to have an address */
    TAILNET_CONNECTING,   /* talking to the control plane */
    TAILNET_CONNECTED,    /* on the tailnet, ip is valid */
    TAILNET_ERROR,        /* MicroLink could not start; retried with backoff */
} tailnet_state_t;

typedef struct {
    tailnet_state_t state;
    bool enabled;
    bool key_set;         /* the key itself is never handed out */
    char ip[16];          /* tailnet address, "" until connected */
    int peers;
} tailnet_status_t;

/* Load settings from NVS and start the supervisor task. Call after
 * netmgr_start(): it listens to the same Wi-Fi and IP events. */
esp_err_t tailnet_start(void);

void tailnet_get(tailnet_status_t *out);

/* Short word for the status page and logs. */
const char *tailnet_state_name(tailnet_state_t s);

/* Change settings. `key` NULL or "" keeps the stored key; otherwise it must
 * look like a Tailscale auth key (tskey-...). `forget` erases the key and
 * this board's tailnet identity, and switches Tailscale off. Takes effect
 * without a restart. ESP_ERR_INVALID_ARG for a malformed key. */
esp_err_t tailnet_configure(bool enabled, const char *key, bool forget);

#endif /* CONFIG_UPSA_TAILSCALE */

#endif /* UPSA_TAILNET_H */
