/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/isp_core.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "tc358743.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sdkconfig.h"

#define CAPTURE_LOG_TAG "p4kvm"

/*
 * Pixel-format descriptor: every format-dependent constant of the capture
 * pipeline is derived HERE and only here from the Kconfig pipeline choice.
 * capture_hw.c / capture_mjpeg.c / capture_testpat.c consume these macros so a
 * new pipeline (e.g. the rev >= 3.0 CSI color converter) is added in one place.
 *
 * YUV422 frames are 2/3 the size of RGB888, so a third CSI buffer fits the
 * same PSRAM budget and lowers the chance of the encoder reading a buffer the
 * CSI DMA is about to overwrite after a stall.
 */
#if CONFIG_P4KVM_PIPELINE_YUV422
#define CAPTURE_PIXEL_BPP 16u
#define CAPTURE_CSI_DATA_TYPE 0x1eu /* CSI-2 YUV422 8-bit */
#define CAPTURE_CAM_COLOR CAM_CTLR_COLOR_YUV422_UYVY
#define CAPTURE_ISP_COLOR ISP_COLOR_YUV422
#define CAPTURE_JPEG_SRC_TYPE JPEG_ENCODE_IN_FORMAT_YUV422
#define CAPTURE_JPEG_SUBSAMPLE JPEG_DOWN_SAMPLING_YUV422 /* encoder requires 4:2:2 for YUV422 input */
#define CAPTURE_PIPELINE_NAME "yuv422"
/*
 * The luma/chroma reorder the encoder needs (TC358743 lands luma-first
 * [Y,C,Y,C]; the P4 JPEG encoder consumes chroma-first) is done for free in
 * the CSI bridge write path via esp_cam_ctlr_csi_config_t::byte_swap_en - see
 * the comment on s_csi_stack_create() in capture_hw.c. CAPTURE_NEEDS_REORDER
 * gates the alternative software path (a BitScrambler DMA pass,
 * main/uyvy_to_yvyu.bsasm, kept for reference) which stays OFF: it was
 * measured at ~28 MB/s (147 ms per 1080p frame), far too slow for 30 fps.
 * Only set this to 1 (and re-enable the bsasm in main/CMakeLists.txt) for a
 * future reorder the hardware byte swap cannot express.
 */
#define CAPTURE_NEEDS_REORDER 0
#define CAPTURE_FB_COUNT 3
#else
#define CAPTURE_PIXEL_BPP 24u
#define CAPTURE_CSI_DATA_TYPE 0x24u /* CSI-2 RGB888 */
#define CAPTURE_CAM_COLOR CAM_CTLR_COLOR_RGB888
#define CAPTURE_ISP_COLOR ISP_COLOR_RGB888
#define CAPTURE_JPEG_SRC_TYPE JPEG_ENCODE_IN_FORMAT_RGB888
#define CAPTURE_JPEG_SUBSAMPLE JPEG_DOWN_SAMPLING_YUV420
#define CAPTURE_PIPELINE_NAME "rgb888"
#define CAPTURE_NEEDS_REORDER 0
#define CAPTURE_FB_COUNT 2
#endif

/**
 * Shared capture state for codec tasks (lives in capture_hw.c or
 * capture_testpat.c).
 *
 * Cache-coherency invariant: in HDMI capture mode the fb ring is touched by
 * DMA only (CSI writes, BitScrambler/JPEG read); it is flushed+invalidated
 * once after allocation and never CPU-accessed again. Any future CPU read or
 * write of fb[] pixel data must bring back per-frame esp_cache_msync, or the
 * JPEG driver's internal C2M write-back will flush stale lines over live DMA
 * data. The test-pattern source CPU-writes the ring and therefore does its
 * own per-frame write-back before publishing.
 */
typedef struct {
    uint32_t hres;
    uint32_t vres;
    size_t frame_bytes;
    void *fb[CAPTURE_FB_COUNT];
    void *volatile done_fb;
    volatile int ping_fb_idx;
    SemaphoreHandle_t csi_done_sem;
    tc358743_t *tc; /**< NULL in test-pattern mode (no bridge attached). */
    volatile uint32_t csi_dma_done_irqs;
    volatile uint32_t csi_get_new_irqs;
} capture_ctx_t;

/**
 * LDO, I2C, TC358743, frame buffers, CSI, ISP bypass, then HDMI lock and esp_cam start.
 * Returns a pointer to internal storage; valid until the task exits.
 */
capture_ctx_t *capture_hw_init_start(void);

/**
 * After HDMI loss (host sleep): stop CSI, HDMI HPD cycle, re-kick TC358743 MIPI, P4 bridge regs, esp_cam start.
 * Safe to call from the capture task when frames have stalled; throttled by the caller.
 * @param deep false: HPD hotplug cycle only. true: full TC358743 register re-init (escalation
 *             when repeated hotplug cycles fail, e.g. after the source slept for a long time).
 */
esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c, bool deep);

/**
 * Non-destructive CSI re-kick for the case where the TC358743 still reports a
 * locked HDMI input (TMDS+SYNC) but the P4 CSI-2 bridge has stalled (no line
 * packets / no DMA completions). Restarts esp_cam and rewrites the TC's CSI-2
 * TX start, WITHOUT touching HPD/EDID/TMDS - so the HDMI source is not forced
 * to re-enumerate. Use this instead of capture_hw_hdmi_recover() whenever the
 * input is still locked; only fall back to the HPD-cycling recover when the
 * TC has actually lost lock.
 */
esp_err_t capture_hw_csi_rekick(capture_ctx_t *c);

/**
 * Last-resort recovery for a wedged MIPI D-PHY: destroys and recreates the whole
 * esp_cam CSI controller + ISP processor (the only way to re-init the D-PHY
 * receiver), then re-issues the TC358743 CSI start without touching HPD. Use
 * when repeated capture_hw_csi_rekick() calls fail to restore frames while the
 * TC still reports a locked input.
 */
esp_err_t capture_hw_csi_full_reinit(capture_ctx_t *c);

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes);

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp);

/** Animated test-pattern source: same ring/semaphore contract, no TC358743/CSI hardware. */
capture_ctx_t *capture_testpat_init_start(void);

void capture_mjpeg_run(capture_ctx_t *c);
