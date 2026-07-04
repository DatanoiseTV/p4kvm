/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Video pipeline counters, published by the capture task roughly every 2 s and
 * read lock-free by the HTTP /stats handler. All fields are 32-bit (single
 * aligned stores on this core), so a reader sees a possibly mixed but never
 * torn snapshot - fine for diagnostics.
 */
typedef struct {
    uint32_t cap_frames;    /**< CSI DMA frames completed since boot. */
    uint32_t enc_frames;    /**< JPEG frames published since boot. */
    uint32_t enc_errors;    /**< jpeg_encoder_process failures since boot. */
    uint32_t cap_fps_x10;   /**< CSI frame rate over the last window, x10. */
    uint32_t enc_fps_x10;   /**< Encoded frame rate over the last window, x10. */
    uint32_t bs_us;         /**< Mean BitScrambler reorder time last window (0 on RGB888). */
    uint32_t enc_us;        /**< Mean hardware JPEG encode time last window. */
    uint32_t jpeg_bytes;    /**< Mean published JPEG size last window. */
    uint32_t recoveries;    /**< HDMI/CSI recovery attempts since boot. */
    uint32_t hdmi_locked;   /**< 1 when SYS_STATUS reports TMDS + sync. */
    uint32_t sys_status;    /**< Raw TC358743 SYS_STATUS from the last check. */
} video_stats_t;

extern volatile video_stats_t g_video_stats;

/** Compile-time pipeline name for /stats ("rgb888" or "yuv422-bs"). */
const char *video_stats_pipeline_name(void);

#ifdef __cplusplus
}
#endif
