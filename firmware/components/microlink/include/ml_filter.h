/*
 * ml_filter: on-device enforcement of the tailnet packet filter (ACLs).
 *
 * ups-driver local patch (2026-11-25): ported from upstream MicroLink
 * 82f05fd and a379946 ("Enforce the tailnet packet filter (ACLs) on the
 * device"), adapted to this copy: IPv4 only (inner IPv6 is dropped before
 * this runs) and no advertised routes (the board takes and offers none), so
 * the only local destination is the board's own tailnet address.
 *
 * The control plane sends the tailnet policy as MapResponse.PacketFilter
 * (array of tailcfg.FilterRule) or, on newer servers, PacketFilters (map of
 * named rule sets). Like tailscaled's wgengine/filter, inbound packets from
 * the tunnel are accepted only if a rule allows them:
 *   - TCP: only new connections (SYN without ACK) are checked; other TCP
 *     packets belong to connections that had to start with a checked SYN.
 *   - UDP: replies to flows this device started are accepted (flow table).
 *   - ICMP: echo replies and errors are accepted; other ICMP needs a rule.
 * Until the first policy arrives nothing new is accepted (replies and ICMP
 * errors are), as tailscaled starts with an allow-nothing filter. A rule set
 * that cannot be applied (malformed, too large) allows nothing.
 *
 * In every mode, OFF included, a packet must be addressed to the board's
 * tailnet IP once that is known ("destination not allowed" in tailscaled).
 *
 * The core is pure C (plus cJSON) so tools/test/test_ml_filter.py can run it
 * on the host.
 */
#ifndef ML_FILTER_H
#define ML_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ML_FILTER_MODE_OFF = 0,     /* accept everything (only the destination check) */
    ML_FILTER_MODE_LOG,         /* evaluate and count, but accept */
    ML_FILTER_MODE_ENFORCE,     /* drop what the policy doesn't allow */
} ml_filter_mode_t;

typedef struct {
    uint32_t accepted;
    uint32_t dropped;           /* would-drop count in LOG mode */
    uint32_t rules;             /* rules in the active policy */
    bool     configured;        /* a policy has been received */
} ml_filter_stats_t;

struct cJSON;

/* Set up the filter (idempotent). */
void ml_filter_init(ml_filter_mode_t mode);
/* Free the policy and flow state (microlink_destroy). Back to allow-nothing. */
void ml_filter_reset(void);

/*
 * Apply the filter fields of one MapResponse (full or delta), as tailscaled
 * does: "PacketFilter" absent or null = unchanged, an array = replaces every
 * set with it as "base"; then "PacketFilters" {name: rules|null} updates
 * named sets (null deletes, the key "*" clears all first). Returns true if
 * the policy changed.
 */
bool ml_filter_update_from_map(const struct cJSON *map_response);

/* The board's tailnet IPv4 address, host byte order (0 = not known yet: no
 * destination check). */
void ml_filter_set_local(uint32_t self_ip);

/* Inbound IPv4 packet from the tunnel (full IP packet, len bytes).
 * Returns true if it may be delivered. */
bool ml_filter_check_inbound(const uint8_t *pkt, size_t len, uint32_t now_ms);

/* Outbound IPv4 packet into the tunnel (at least the first 28 bytes):
 * records UDP flows so replies are accepted. */
void ml_filter_note_outbound(const uint8_t *pkt, size_t len, uint32_t now_ms);

void ml_filter_get_stats(ml_filter_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ML_FILTER_H */
