/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Derive a short-lived TURN credential from a coturn static-auth-secret
 * (RFC 5766 "TURN REST API" / coturn `use-auth-secret`). The username is an
 * expiry unix timestamp (now + ttl_s); the password is
 * base64(HMAC-SHA1(secret, username)). The same pair authenticates both the
 * device's esp_peer agent and the browser, and expires on its own, so the
 * long-term secret never leaves the device.
 *
 * Requires the clock to be set (SNTP); returns ESP_ERR_INVALID_STATE if time
 * is not yet synced, ESP_ERR_INVALID_SIZE if a buffer is too small.
 */
esp_err_t turn_cred_make(const char *secret, int ttl_s,
                         char *user, size_t user_sz,
                         char *pass, size_t pass_sz);

#ifdef __cplusplus
}
#endif
