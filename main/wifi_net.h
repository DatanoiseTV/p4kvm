/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/** WiFi STA via the companion radio (esp_wifi_remote); no-op when P4KVM_WIFI_ENABLE is off. */
esp_err_t wifi_net_init(void);
