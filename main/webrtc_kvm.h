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
 * One-time init of the WebRTC KVM session manager. No-op (and always available)
 * when P4KVM_WEBRTC_ENABLE is off. Safe to call once at startup.
 */
void webrtc_kvm_init(void);

/**
 * Handle a browser SDP offer and produce the device's answer. Synchronous:
 * tears down any existing session, opens a new peer (role: controlled / answerer,
 * H.264 send-only video + a reliable HID data channel), feeds the offer, and
 * waits (bounded) for the answer SDP and ICE candidates.
 *
 * @param offer      Raw offer SDP (the browser's pc.localDescription.sdp).
 * @param offer_len  Length of @p offer.
 * @param resp       Output buffer, filled with a JSON object
 *                   {"sdp":"<answer>","candidates":["candidate:...",...]}
 *                   (NUL-terminated).
 * @param resp_cap   Capacity of @p resp.
 * @param resp_len   Set to the JSON length written (excluding NUL).
 *
 * @return ESP_OK on success; ESP_ERR_* if disabled, busy, or negotiation failed.
 */
esp_err_t webrtc_kvm_handle_offer(const char *offer, size_t offer_len, char *resp, size_t resp_cap, size_t *resp_len);

/**
 * Build the ICE-server list the browser should use, as JSON:
 *   {"iceServers":[{"urls":"stun:..."},
 *                  {"urls":"turn:host:port","username":"...","credential":"..."}],
 *    "ttl":3600}
 * The TURN entry is present only when a TURN server + secret are configured;
 * its credentials are freshly derived (short-lived) on each call so the browser
 * and device relay through the same coturn. Served by GET /webrtc/ice.
 *
 * @return ESP_OK on success (TURN entry included only if configured + clock set).
 */
esp_err_t webrtc_kvm_ice_config_json(char *resp, size_t resp_cap, size_t *resp_len);

/** True while a WebRTC viewer is connected (data path up). */
bool webrtc_kvm_connected(void);

/** Short state string for /stats: "off", "idle", "connecting", "connected". */
const char *webrtc_kvm_state_str(void);

#ifdef __cplusplus
}
#endif
