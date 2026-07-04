/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NVS-backed runtime settings with Kconfig fallbacks, editable from the web
 * UI (GET/POST /config). String getters copy into the caller's buffer and
 * always NUL-terminate; they return the fallback when NVS has no value.
 * Changes apply on the next boot (network/GPIO subsystems read them once at
 * init), so the UI saves and restarts.
 */

void runtime_cfg_get_str(const char *key, const char *fallback, char *buf, size_t sz);
int32_t runtime_cfg_get_i32(const char *key, int32_t fallback);
esp_err_t runtime_cfg_set_str(const char *key, const char *value);
esp_err_t runtime_cfg_set_i32(const char *key, int32_t value);

/* NVS keys (<= 15 chars). Values: strings unless noted. */
#define RT_KEY_WIFI_SSID "wifi_ssid"
#define RT_KEY_WIFI_PASS "wifi_pass"
#define RT_KEY_WG_PRIV "wg_priv"
#define RT_KEY_WG_PEER_PUB "wg_pub"
#define RT_KEY_WG_PSK "wg_psk"
#define RT_KEY_WG_ENDPOINT "wg_ep"
#define RT_KEY_WG_PORT "wg_port"     /* i32 */
#define RT_KEY_WG_LOCAL_IP "wg_ip"
#define RT_KEY_WG_MASK "wg_mask"
#define RT_KEY_WG_KEEPALIVE "wg_ka"  /* i32 */
#define RT_KEY_ATX_POWER "atx_pwr"   /* i32 gpio, -1 off */
#define RT_KEY_ATX_RESET "atx_rst"   /* i32 gpio, -1 off */
#define RT_KEY_ATX_ACTIVE_HIGH "atx_lvl" /* i32 0/1 */

#ifdef __cplusplus
}
#endif
