/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UPSA_FONT5X7_H
#define UPSA_FONT5X7_H

#include <stdint.h>

#define FONT_WIDTH        5
#define FONT_ADVANCE      6   /* glyph plus one column of spacing */
#define FONT_GLYPH_COUNT  43

extern const uint8_t font5x7[FONT_GLYPH_COUNT][FONT_WIDTH];

/* Map a character to its glyph index. Folds lowercase, falls back to '?'. */
int font_index(char c);

#endif /* UPSA_FONT5X7_H */
