/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/**
 * Start the WireGuard tunnel task (no-op when P4KVM_WG_ENABLE is off or the
 * private key is empty). Call after network init; the task waits for an IP,
 * syncs time via SNTP (handshakes need valid wall-clock), then connects and
 * supervises the peer, reconnecting when the handshake goes stale.
 */
esp_err_t wireguard_net_start(void);

/** "off", "starting", "handshaking", "up", or "retrying" - for /stats. */
const char *wireguard_net_status_str(void);
