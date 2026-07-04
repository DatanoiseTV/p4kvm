/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define JPEG_SLOT_COUNT 3

typedef struct {
    SemaphoreHandle_t mutex;
    /** Serializes MJPEG sends (TCP chunk ordering / shared state). */
    SemaphoreHandle_t xmit_mutex;
    /** Incremented after each successful JPEG publish; HTTP waits until this advances. */
    volatile uint32_t frame_seq;
    /** Counting semaphore: encoder gives once per connected /stream client per new frame. */
    SemaphoreHandle_t frame_ready_sem;
    /** Triple-buffered JPEG output; encoder writes a slot with slot_ref[i]==0 and i != front_idx. */
    uint8_t *jpeg_buf[JPEG_SLOT_COUNT];
    /** Per-slot size of last encode into that slot (valid for published slot). */
    size_t jpeg_len[JPEG_SLOT_COUNT];
    /** Streams currently sending multipart body from jpeg_buf[i] (encoder must not reuse until 0). */
    uint8_t slot_ref[JPEG_SLOT_COUNT];
    /** Latest finished encode slot, or -1 before first frame. */
    int front_idx;
    size_t jpeg_cap;
    /** Encoder quality 1–100; writable at runtime (see `/jpeg-quality`). */
    volatile uint8_t jpeg_quality;
} jpeg_frame_slot_t;

extern jpeg_frame_slot_t g_jpeg_frame;

void jpeg_frame_stream_enter(void);
void jpeg_frame_stream_leave(void);
/** Number of connected /stream clients. */
int jpeg_frame_stream_clients(void);
/** Call after publishing a new JPEG (updates seq + notifies stream tasks). */
void jpeg_frame_notify_new_frame(void);
/** Caller must hold @ref jpeg_frame_slot_t.mutex. Returns -1 if no slot free yet. */
int jpeg_frame_pick_encode_slot(void);

/** Load JPEG quality from NVS if present (1–100); no-op if missing or invalid. Call after defaulting from Kconfig. */
void jpeg_quality_load_from_nvs(void);
/** Persist quality for next boot; @param q must be 1–100. */
esp_err_t jpeg_quality_save_to_nvs(uint8_t q);
