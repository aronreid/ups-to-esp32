/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UPSA_OTA_NOTES_H
#define UPSA_OTA_NOTES_H

#include <stddef.h>

#define NOTES_MAX_ITEMS  6
#define NOTES_ITEM_CHARS 160

/* Bullets under "## What changed" in a GitHub release body, as
 * newline-separated items: at most NOTES_MAX_ITEMS, each cut to about
 * NOTES_ITEM_CHARS. out is "" when there is no such list. */
void ota_abridge_notes(const char *body, char *out, size_t cap);

#endif
