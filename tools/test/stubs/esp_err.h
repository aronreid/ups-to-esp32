/* SPDX-License-Identifier: GPL-3.0-or-later
 * Syntax-only stub. See stubs/README.md before trusting a pass. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERROR_CHECK(x)      ((void)(x))
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return ""; }
#endif
