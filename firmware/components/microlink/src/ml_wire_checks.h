/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Checks on what the control server and the DERP relay send, before it is
 * trusted. Header-only so tools/test/test_ml_wire.py can compile it on the
 * host. local patch (2026-11-25), from upstream MicroLink af3a58e, dd29cf6,
 * d6493c0 and debc839.
 */
#ifndef UPSA_ML_WIRE_CHECKS_H
#define UPSA_ML_WIRE_CHECKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The HTTP upgrade answer must START with a 101 status line
 * ("HTTP/1.1 101 ..."). Looking for "101" anywhere in the response accepted
 * any error page that happened to contain those digits. */
static inline bool ml_http_status_is_101(const char *resp, size_t len)
{
    static const char pfx[] = "HTTP/1.";
    if (!resp || len < 12) return false;
    for (size_t i = 0; i < sizeof(pfx) - 1; i++) if (resp[i] != pfx[i]) return false;
    if (resp[7] != '0' && resp[7] != '1') return false;
    if (resp[8] != ' ' || resp[9] != '1' || resp[10] != '0' || resp[11] != '1') return false;
    return len == 12 || resp[12] == ' ' || resp[12] == '\r';
}

/* Noise IK message 2 is the responder's ephemeral key and an empty payload's
 * tag: exactly 48 bytes. A longer one made the decrypt write its excess
 * through a NULL output pointer, before the server was authenticated. */
#define ML_NOISE_MSG2_LEN 48u

/* The Noise frame header is not authenticated and its length decides an
 * allocation before the MAC is checked. Tailscale's controlbase never sends
 * a frame over 4 KB; 16 KB is the limit here. */
#define ML_NOISE_MAX_CT_LEN (16384u + 16u)
static inline bool ml_noise_ct_len_ok(size_t ct_len)
{
    return ct_len >= 16 && ct_len <= ML_NOISE_MAX_CT_LEN;
}

/* A control-plane read that has started has to finish: within STALL ms of
 * the last progress and TOTAL ms of its first byte. Each retry can block for
 * a whole SO_RCVTIMEO (up to 60 s), so the old 300-retry count allowed
 * hours. */
#define ML_COORD_RECV_STALL_MS 30000u
#define ML_COORD_RECV_TOTAL_MS 90000u
static inline bool ml_coord_read_overdue(uint64_t start_ms, uint64_t progress_ms, uint64_t now_ms)
{
    return now_ms - progress_ms > ML_COORD_RECV_STALL_MS || now_ms - start_ms > ML_COORD_RECV_TOTAL_MS;
}

/* A DERP RecvPacket frame is the 32-byte source key and a non-empty packet. */
static inline bool ml_derp_recv_packet_len_ok(uint32_t len)
{
    return len > 32;
}

#endif
