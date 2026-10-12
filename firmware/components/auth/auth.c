/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "auth";

/* Same namespace as the Wi-Fi credentials, so netmgr_factory_reset -- the
 * five-press BOOT gesture -- erases the login with them. */
#define NVS_NS      "upsa"
#define KEY_USER    "auth_user"
#define KEY_SALT    "auth_salt"
#define KEY_HASH    "auth_hash"

/* Salted and iterated, so a copy of the flash does not hand over a password
 * people may use elsewhere. The S3 hashes in hardware: 4096 rounds is a few
 * milliseconds per check, and a guesser pays it every time. */
#define HASH_ROUNDS 4096

static authc_t s_a;
static SemaphoreHandle_t s_lock;

static void hash_pass(const uint8_t salt[AUTH_SALT_LEN], const char *pass,
                      uint8_t out[AUTH_HASH_LEN])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, AUTH_SALT_LEN);
    mbedtls_sha256_update(&c, (const unsigned char *)pass, strlen(pass));
    mbedtls_sha256_finish(&c, out);
    for (int i = 0; i < HASH_ROUNDS; i++) {
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, out, AUTH_HASH_LEN);
        mbedtls_sha256_update(&c, salt, AUTH_SALT_LEN);
        mbedtls_sha256_finish(&c, out);
    }
    mbedtls_sha256_free(&c);
}

static int64_t now_us(void) { return esp_timer_get_time(); }

esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_a, 0, sizeof(s_a));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   /* nothing saved */
    char user[AUTH_USER_MAX + 1] = "";
    uint8_t salt[AUTH_SALT_LEN], hash[AUTH_HASH_LEN];
    size_t ul = sizeof(user), sl = sizeof(salt), hl = sizeof(hash);
    if (nvs_get_str(h, KEY_USER, user, &ul) == ESP_OK &&
        nvs_get_blob(h, KEY_SALT, salt, &sl) == ESP_OK && sl == AUTH_SALT_LEN &&
        nvs_get_blob(h, KEY_HASH, hash, &hl) == ESP_OK && hl == AUTH_HASH_LEN && user[0]) {
        authc_set(&s_a, user, salt, hash);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "login %s", authc_enabled(&s_a) ? "ON: changes need a username and password"
                                                  : "off (optional; set one on the web page)");
    return ESP_OK;
}

bool auth_enabled(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = authc_enabled(&s_a);
    xSemaphoreGive(s_lock);
    return on;
}

esp_err_t auth_set_credentials(const char *user, const char *pass)
{
    bool clear = !user || !user[0];
    if (!clear && !authc_shape_ok(user, pass)) return ESP_ERR_INVALID_ARG;

    uint8_t salt[AUTH_SALT_LEN] = {0}, hash[AUTH_HASH_LEN] = {0};
    if (!clear) {
        esp_fill_random(salt, sizeof(salt));
        hash_pass(salt, pass, hash);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (clear) {
        nvs_erase_key(h, KEY_USER);
        nvs_erase_key(h, KEY_SALT);
        nvs_erase_key(h, KEY_HASH);
    } else {
        nvs_set_str(h, KEY_USER, user);
        nvs_set_blob(h, KEY_SALT, salt, sizeof(salt));
        nvs_set_blob(h, KEY_HASH, hash, sizeof(hash));
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    authc_set(&s_a, clear ? "" : user, salt, hash);     /* also logs everyone out */
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "login %s", clear ? "turned off" : "set");
    return ESP_OK;
}

static auth_result_t from_core(authc_result_t r)
{
    return r == AUTHC_OK ? AUTH_OK : r == AUTHC_LOCKED ? AUTH_LOCKED : AUTH_BAD;
}

auth_result_t auth_check(const char *user, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    authc_result_t r = authc_check(&s_a, user, pass, now_us(), hash_pass);
    xSemaphoreGive(s_lock);
    return from_core(r);
}

auth_result_t auth_login(const char *user, const char *pass,
                         char token[AUTH_TOKEN_LEN + 1])
{
    uint8_t rnd[AUTH_TOKEN_LEN / 2];
    esp_fill_random(rnd, sizeof(rnd));
    for (size_t i = 0; i < sizeof(rnd); i++) sprintf(token + 2 * i, "%02x", rnd[i]);
    token[AUTH_TOKEN_LEN] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t = now_us();
    authc_result_t r = authc_check(&s_a, user, pass, t, hash_pass);
    if (r == AUTHC_OK) authc_issue(&s_a, token, t);
    xSemaphoreGive(s_lock);
    if (r != AUTHC_OK) token[0] = '\0';
    return from_core(r);
}

bool auth_token_ok(const char *token)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = authc_token_ok(&s_a, token, now_us());
    xSemaphoreGive(s_lock);
    return ok;
}

void auth_logout(const char *token)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    authc_revoke(&s_a, token);
    xSemaphoreGive(s_lock);
}

authc_nut_t auth_nut_write(const char *user, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    authc_nut_t r = authc_nut_write(&s_a, user, pass, now_us(), hash_pass);
    xSemaphoreGive(s_lock);
    return r;
}

bool auth_origin_ok(const char *origin, const char *host)
{
    return authc_origin_ok(origin, host);
}
