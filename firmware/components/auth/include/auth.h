/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* The optional login.
 *
 * OFF BY DEFAULT, and off means exactly what the board did before: anyone who
 * can reach it can read it and change it. Setting a username and password --
 * during Wi-Fi setup or later on the page -- turns it on, and from then on
 * every change needs them: the web page's writes (UPS commands, Wi-Fi,
 * updates, reboots) and NUT's INSTCMD and SET VAR. Reading never needs them,
 * so Home Assistant, a NAS and Prometheus keep working with no setup.
 *
 * The five-press BOOT reset clears the login along with Wi-Fi: forgetting the
 * password costs a trip to the board, never the board.
 *
 * Plain HTTP and plain NUT: the password crosses the LAN readable, as it does
 * with upsd. This keeps honest people and stray scripts out; it does not
 * defeat someone capturing the network. */
#include "esp_err.h"
#include "auth_core.h"
#include <stdbool.h>

typedef enum { AUTH_OK, AUTH_BAD, AUTH_LOCKED } auth_result_t;

esp_err_t auth_init(void);                  /* load from NVS; call after nvs_flash_init */
bool auth_enabled(void);

/* Set, change or (user NULL or "") clear the login. ESP_ERR_INVALID_ARG if the
 * username or password is not one this board can store (see authc_shape_ok). */
esp_err_t auth_set_credentials(const char *user, const char *pass);

/* Check a username and password; on success, token receives a new login token
 * of AUTH_TOKEN_LEN hex characters for the web page. */
auth_result_t auth_login(const char *user, const char *pass,
                         char token[AUTH_TOKEN_LEN + 1]);
auth_result_t auth_check(const char *user, const char *pass);
bool auth_token_ok(const char *token);
void auth_logout(const char *token);

authc_nut_t auth_nut_write(const char *user, const char *pass);
bool auth_origin_ok(const char *origin, const char *host);
