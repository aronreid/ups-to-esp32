/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal esp_log.h so the descriptor parser can be compiled and tested on a
 * host machine. The parser is plain C apart from logging, so it can be checked
 * against a real captured descriptor without ESP-IDF or hardware. */
#ifndef HOSTTEST_ESP_LOG_H
#define HOSTTEST_ESP_LOG_H
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif
