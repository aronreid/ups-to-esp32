/* SPDX-License-Identifier: MIT (local patch to vendored MicroLink, GitLab #30)
 *
 * Read exactly `len` bytes from a stream that can return short reads, such as
 * mbedtls_ssl_read(), which returns at most what is left of the current TLS
 * record. Pure, so tools/test/test_ml_read_full.py compiles it on the host.
 *
 * The DERP reader used to read its 5-byte frame header with one call and treat
 * 1 to 4 bytes as fatal. A relay that flushes several frames together can put
 * a TLS record boundary inside a header, and each time that cost a full relay
 * reconnect.
 *
 * rd(ctx, buf, n) returns the bytes read (> 0), ML_RD_AGAIN when nothing is
 * available yet, 0 when the peer closed, or another negative error.
 * wait(ctx) runs after ML_RD_AGAIN once at least one byte of the item has been
 * read (or always, with may_idle false): it sleeps and returns 0 to go on, or
 * a nonzero code to give up with (stopped, timed out).
 *
 * Returns (int)len when complete; 0 when may_idle and nothing at all was read
 * (no frame yet: the caller polls again later); otherwise the error from rd,
 * -1 for a close part way through an item, or wait's code.
 */
#ifndef ML_READ_FULL_H
#define ML_READ_FULL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ML_RD_AGAIN (-0x7FFF)

typedef int (*ml_rd_fn)(void *ctx, uint8_t *buf, size_t len);
typedef int (*ml_rd_wait_fn)(void *ctx);

static inline int ml_read_full(void *ctx, ml_rd_fn rd, ml_rd_wait_fn wait,
                               uint8_t *buf, size_t len, bool may_idle)
{
    size_t have = 0;
    while (have < len) {
        int n = rd(ctx, buf + have, len - have);
        if (n == ML_RD_AGAIN) {
            if (have == 0 && may_idle) return 0;
            int w = wait(ctx);
            if (w) return w;
            continue;
        }
        if (n == 0) {
            /* Closed. Before any byte of an optional item this is what the
             * reader always reported (0, "nothing"); part way through, the
             * stream is out of step and the connection has to go. */
            return (have == 0 && may_idle) ? 0 : -1;
        }
        if (n < 0) return n;
        if ((size_t)n > len - have) return -1;     /* a broken rd; never trust it */
        have += (size_t)n;
    }
    return (int)len;
}

#endif
