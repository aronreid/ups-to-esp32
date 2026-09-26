/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Release-notes abridging for the update card. Plain C with no ESP-IDF
 * dependency, so tools/test/test_release_notes.py compiles and runs it on the
 * host against real release bodies.
 */
#include "ota_notes.h"
#include <stdbool.h>
#include <string.h>

/* Pull the bullets under "## What changed" out of a release body, as
 * newline-separated items. release.yml writes that heading above the tag's own
 * notes. A bullet may wrap onto indented lines; the list ends at the first
 * blank line or heading after it starts. Items are cut at NOTES_ITEM_CHARS on
 * a character boundary: the full text is one link away. */
void ota_abridge_notes(const char *body, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = body ? strstr(body, "## What changed") : NULL;
    if (!p) return;
    p = strchr(p, '\n');
    int items = 0, len = 0;
    size_t o = 0;
    bool in_list = false, in_item = false, cut = false;
    while (p && *p) {
        p++;                                    /* past the newline */
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        const char *t = p;
        while (t < p + n && (*t == ' ' || *t == '\t')) t++;
        size_t tn = (size_t)(p + n - t);

        if (tn == 0 || *t == '#') {
            if (in_list) break;                 /* the list is over */
        } else if (tn >= 2 && (t[0] == '-' || t[0] == '*') && t[1] == ' ') {
            if (items == NOTES_MAX_ITEMS) break;
            if (in_item && o < cap - 1) out[o++] = '\n';
            t += 2; tn -= 2;
            in_list = in_item = true; items++; len = 0; cut = false;
        } else if (!in_item) {
            p = eol; continue;                  /* prose before the list */
        } else if (cut) {
            p = eol; continue;                  /* rest of an item already cut */
        } else if (o < cap - 1 && len < NOTES_ITEM_CHARS) {
            out[o++] = ' '; len++;              /* a wrapped continuation */
        }
        if (in_item && tn) {
            for (size_t i = 0; i < tn && o < cap - 1; i++) {
                /* len counts characters, not bytes: only a UTF-8 lead byte
                 * starts one, so a cut never lands inside a glyph. */
                bool lead = ((unsigned char)t[i] & 0xC0) != 0x80;
                if (lead && len >= NOTES_ITEM_CHARS) {
                    if (!cut && o + 3 < cap) {          /* U+2026 ELLIPSIS */
                        out[o++] = (char)0xE2; out[o++] = (char)0x80; out[o++] = (char)0xA6;
                    }
                    cut = true;
                    break;
                }
                out[o++] = t[i];
                if (lead) len++;
            }
        }
        p = eol;
    }
    out[o] = '\0';
}
