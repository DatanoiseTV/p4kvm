/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Optional HDMI audio capture (P4KVM_AUDIO_ENABLE): the TC358743 outputs the
 * source's audio on its I2S pads (it is I2S master; clocks derive from HDMI),
 * jumper-wired to three P4 GPIOs. Streamed to the browser as raw PCM S16LE
 * over the /audio WebSocket.
 */
esp_err_t audio_stream_init(void);

/** "off" (not built/wired), "idle" (no listener), or "streaming" - for /stats. */
const char *audio_stream_status_str(void);

/** /audio WebSocket handler + session bookkeeping (single listener). */
esp_err_t audio_ws_handler(httpd_req_t *req);
void audio_stream_on_sock_close(int sockfd);
/** True when the audio endpoint should be registered. */
bool audio_stream_available(void);
