/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The real ESP_LOGx macros consume their arguments, so the stub must too --
 * otherwise a tag or a helper used only inside a log call looks unused, and
 * -Werror turns that into a false failure.
 */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

static inline void stub_log_sink(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

#define ESP_LOGE(tag, fmt, ...) stub_log_sink((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_log_sink((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) stub_log_sink((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) stub_log_sink((tag), (fmt), ##__VA_ARGS__)

#endif
