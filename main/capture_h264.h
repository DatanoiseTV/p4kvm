/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sink for encoded H.264 access units. Called on the capture task once per
 * encoded frame with an Annex-B NAL stream (SPS+PPS are prepended by the
 * encoder on IDR frames). Must not block for long - it feeds the WebRTC
 * sender directly. @p pts_ms is a millisecond presentation timestamp.
 */
typedef void (*capture_h264_sink_fn)(const uint8_t *nal, size_t len, bool keyframe, uint32_t pts_ms, void *ctx);

/**
 * Register (or clear, with fn == NULL) the encoded-frame sink. Registering a
 * sink is what enables H.264 encoding: with no sink, capture_h264_on_csi_frame
 * is a cheap no-op, so the hardware encoder only runs while a WebRTC viewer is
 * connected. Safe to call from any task.
 */
void capture_h264_set_sink(capture_h264_sink_fn fn, void *ctx);

/**
 * Encode one captured CSI frame (UYVY, @p c->frame_bytes long) to H.264 and
 * hand the result to the registered sink. No-op when no sink is registered.
 * Lazily creates the hardware encoder and re-creates it if the resolution
 * changed. Runs on the capture task, after the JPEG encode of the same frame.
 */
void capture_h264_on_csi_frame(const uint8_t *uyvy, capture_ctx_t *c);

/** Force the next encoded frame to be an IDR (new viewer joined / RTCP PLI). */
void capture_h264_request_keyframe(void);

/** Mean hardware H.264 encode time (µs) over the last window; 0 if idle. */
uint32_t capture_h264_enc_us(void);
/** Mean encoded frame size (bytes) over the last window; 0 if idle. */
uint32_t capture_h264_frame_bytes(void);
/** Transmitted H.264 frames/s x10 over the last window; 0 if idle. */
uint32_t capture_h264_fps_x10(void);

#ifdef __cplusplus
}
#endif
