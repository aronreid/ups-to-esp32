/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NUT's wire format, the parts that are easy to get subtly wrong: whole
 * command lines out of a TCP byte stream, and values quoted the way upsd
 * quotes them.
 *
 * Lines: TCP delivers bytes, not lines. Each recv() used to be taken as whole
 * lines, so a command split across two segments ("LIST VAR u" then "ps\n")
 * ran as two broken commands, and a client that pipelined more than one
 * buffer's worth of GET VARs was guaranteed a split. Its replies then came
 * back out of step with what it had asked. Here only a complete line is ever
 * handed on, and the rest waits for the next read.
 *
 * Kept free of ESP-IDF so tools/test/test_nut_line.py can run it on the host.
 */
#ifndef UPSA_NUT_LINE_H
#define UPSA_NUT_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define NUT_LINE_MAX 256

typedef struct {
    char   buf[NUT_LINE_MAX];
    size_t len;
    bool   overlong;        /* discarding up to the next newline */
} nut_linebuf_t;

/* Called once per complete, non-empty line, without its CR/LF. `overlong`:
 * the line did not fit and `line` is empty; answer it with an error so the
 * client's replies stay in step. Return false to stop (LOGOUT). */
typedef bool (*nut_line_cb)(void *ctx, char *line, bool overlong);

/* Feed `n` received bytes. Returns false if the callback asked to stop. */
static inline bool nut_line_feed(nut_linebuf_t *lb, const char *data, size_t n,
                                 nut_line_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        if (ch == '\r' || ch == '\n') {
            bool go = true;
            if (lb->overlong) {
                lb->buf[0] = '\0';
                go = cb(ctx, lb->buf, true);
            } else if (lb->len) {
                lb->buf[lb->len] = '\0';
                go = cb(ctx, lb->buf, false);
            }
            lb->len = 0;
            lb->overlong = false;
            if (!go) return false;
            continue;
        }
        if (lb->overlong) continue;
        if (lb->len + 1 >= sizeof(lb->buf)) {
            lb->overlong = true;
            lb->len = 0;
            continue;
        }
        lb->buf[lb->len++] = ch;
    }
    return true;
}

/* `src` as the inside of a NUT quoted string: " and \ escaped with a
 * backslash, as upsd does. A UPS's model or serial can carry either, and
 * unescaped they end the value early and break the client's parse of the
 * line, or of the whole list. Truncates rather than splitting an escape. */
static inline void nut_quote(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (!cap) return;
    for (; src && *src; src++) {
        bool esc = (*src == '"' || *src == '\\');
        if (o + (esc ? 2 : 1) >= cap) break;
        if (esc) dst[o++] = '\\';
        dst[o++] = *src;
    }
    dst[o] = '\0';
}

#endif
