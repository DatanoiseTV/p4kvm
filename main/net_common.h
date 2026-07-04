/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/** esp_netif + default event loop (idempotent). Call before ethernet/wifi init. */
esp_err_t net_common_init(void);

/** mDNS hostname + _http._tcp service on all interfaces. Call after link inits. */
void net_mdns_init(void);
