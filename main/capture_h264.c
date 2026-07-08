/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware H.264 encode path for the WebRTC video track. The ESP32-P4 has a
 * dedicated H.264 baseline encoder block (separate silicon from the JPEG
 * codec), so this runs alongside the MJPEG path on the same CSI frames without
 * contending for the encoder. It consumes the TC358743's native UYVY directly
 * (ESP_H264_RAW_FMT_UYVY) - no colour conversion - so the input is the same
 * capture ring the JPEG encoder reads.
 *
 * Encoding is demand-gated: with no sink registered (no WebRTC viewer) the
 * per-frame hook returns immediately and the encoder is torn down, so an idle
 * device pays nothing.
 */
#include "capture_h264.h"

#include "sdkconfig.h"

#include "esp_log.h"

static const char *TAG = "p4kvm_h264";

#if CONFIG_P4KVM_WEBRTC_ENABLE

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"

/* IDR cadence: one keyframe per second bounds a new viewer's wait even without
 * an explicit force-IDR path in the encoder. */
#define H264_GOP_FPS_DIV 1

/* Guards sink pointer and lazy-init/teardown of the encoder against
 * capture_h264_set_sink() being called from another task. */
static SemaphoreHandle_t s_lock;

static capture_h264_sink_fn s_sink;
static void *s_sink_ctx;

static esp_h264_enc_handle_t s_enc;
static uint16_t s_enc_w, s_enc_h;
static uint8_t *s_out;             /* DMA-capable encoded-output buffer */
static uint32_t s_out_cap;
static volatile bool s_want_keyframe;

/* Rolling stats for /stats (published each ~2 s window by the caller's cadence;
 * we keep simple last-window means here). */
static uint64_t s_win_us_sum;
static uint32_t s_win_frames;
static uint64_t s_win_bytes_sum;
static int64_t s_win_start_us;
static uint32_t s_enc_us, s_frame_bytes, s_fps_x10;

static void h264_stats_tick(int64_t now_us, uint32_t enc_us, uint32_t out_sz)
{
    s_win_us_sum += enc_us;
    s_win_bytes_sum += out_sz;
    s_win_frames++;
    if (s_win_start_us == 0) {
        s_win_start_us = now_us;
        return;
    }
    int64_t span = now_us - s_win_start_us;
    if (span < (int64_t)2 * 1000000) {
        return;
    }
    s_enc_us = (uint32_t)(s_win_us_sum / s_win_frames);
    s_frame_bytes = (uint32_t)(s_win_bytes_sum / s_win_frames);
    s_fps_x10 = (uint32_t)((uint64_t)s_win_frames * 10000000ull / (uint64_t)span);
    s_win_us_sum = 0;
    s_win_bytes_sum = 0;
    s_win_frames = 0;
    s_win_start_us = now_us;
}

static void h264_encoder_destroy(void)
{
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    if (s_out) {
        esp_h264_free(s_out);
        s_out = NULL;
        s_out_cap = 0;
    }
    s_enc_w = s_enc_h = 0;
    s_enc_us = s_frame_bytes = s_fps_x10 = 0;
    s_win_us_sum = s_win_bytes_sum = 0;
    s_win_frames = 0;
    s_win_start_us = 0;
}

/* Create the hardware encoder for the given resolution. Caller holds s_lock.
 * Returns false (and logs once) if the resolution is unsupported. */
static bool h264_encoder_create(uint16_t w, uint16_t h)
{
    /* The hardware encoder requires width/height to be multiples of 16 and
     * within 1920x2032. 1280x720 is clean; 1920x1080 is not (1080 % 16 != 0)
     * and would need a padded capture buffer, so reject it here rather than
     * read past the CSI ring. */
    if ((w % 16u) || (h % 16u) || w < 80 || h < 80 || w > 1920 || h > 2032) {
        static bool logged;
        if (!logged) {
            ESP_LOGW(TAG, "H.264 unsupported resolution %ux%u (need 16-aligned, <=1920x2032); WebRTC video disabled",
                     w, h);
            logged = true;
        }
        return false;
    }

    uint8_t fps = 30;
    uint32_t bitrate = (uint32_t)CONFIG_P4KVM_WEBRTC_BITRATE_KBPS * 1000u;
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_UYVY,
        .gop = (uint8_t)(fps / H264_GOP_FPS_DIV),
        .fps = fps,
        .res = {.width = w, .height = h},
        .rc = {.bitrate = bitrate, .qp_min = 25, .qp_max = 45},
    };
    esp_h264_err_t err = esp_h264_enc_hw_new(&cfg, &s_enc);
    if (err != ESP_H264_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed (%d)", err);
        s_enc = NULL;
        return false;
    }
    if (esp_h264_enc_open(s_enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed");
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
        return false;
    }

    /* Worst-case IDR at qp_min can approach the raw luma size; w*h is a safe
     * ceiling for a 4:2:0 baseline stream and still far under the UYVY input. */
    uint32_t cap = (uint32_t)w * (uint32_t)h;
    uint32_t actual = 0;
    s_out = esp_h264_aligned_calloc(16, 1, cap, &actual, ESP_H264_MEM_SPIRAM);
    if (!s_out) {
        ESP_LOGE(TAG, "H.264 output buffer alloc failed (%u B)", (unsigned)cap);
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
        return false;
    }
    s_out_cap = actual ? actual : cap;
    s_enc_w = w;
    s_enc_h = h;
    ESP_LOGI(TAG, "H.264 hardware encoder %ux%u @%ufps, %ukbps, GOP %u", w, h, fps,
             CONFIG_P4KVM_WEBRTC_BITRATE_KBPS, cfg.gop);
    return true;
}

void capture_h264_set_sink(capture_h264_sink_fn fn, void *ctx)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sink = fn;
    s_sink_ctx = ctx;
    if (fn) {
        s_want_keyframe = true; /* first frame to a fresh viewer must be IDR */
    } else {
        /* Last viewer gone: free the encoder so idle costs nothing. */
        h264_encoder_destroy();
    }
    xSemaphoreGive(s_lock);
}

void capture_h264_request_keyframe(void)
{
    s_want_keyframe = true;
}

void capture_h264_on_csi_frame(const uint8_t *uyvy, capture_ctx_t *c)
{
    if (!s_sink || !s_lock || !uyvy || !c) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return; /* set_sink in progress; skip this frame */
    }
    if (!s_sink) {
        xSemaphoreGive(s_lock);
        return;
    }

    /* (Re)create on first frame or a resolution change. */
    if (!s_enc || s_enc_w != c->hres || s_enc_h != c->vres) {
        h264_encoder_destroy();
        if (!h264_encoder_create((uint16_t)c->hres, (uint16_t)c->vres)) {
            xSemaphoreGive(s_lock);
            return;
        }
        s_want_keyframe = false; /* a freshly opened encoder emits IDR first */
    } else if (s_want_keyframe) {
        /* No explicit force-IDR in the encoder: recreate to guarantee the next
         * output is SPS+PPS+IDR. Rare (viewer join / PLI), so the cost is fine. */
        h264_encoder_destroy();
        if (!h264_encoder_create((uint16_t)c->hres, (uint16_t)c->vres)) {
            xSemaphoreGive(s_lock);
            return;
        }
        s_want_keyframe = false;
    }

    uint32_t pts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_h264_enc_in_frame_t in = {
        .raw_data = {.buffer = (uint8_t *)uyvy, .len = (uint32_t)c->frame_bytes},
        .pts = pts_ms,
    };
    esp_h264_enc_out_frame_t out = {
        .raw_data = {.buffer = s_out, .len = s_out_cap},
    };

    int64_t t0 = esp_timer_get_time();
    esp_h264_err_t err = esp_h264_enc_process(s_enc, &in, &out);
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_H264_ERR_OK) {
        static uint32_t s_err_logs;
        if ((s_err_logs++ % 64u) == 0u) {
            ESP_LOGW(TAG, "esp_h264_enc_process failed (%d)", err);
        }
        xSemaphoreGive(s_lock);
        return;
    }

    bool keyframe = (out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I);
    h264_stats_tick(t1, (uint32_t)(t1 - t0), out.length);

    capture_h264_sink_fn fn = s_sink;
    void *ctx = s_sink_ctx;
    /* Deliver while holding the lock so a concurrent set_sink(NULL) cannot free
     * the output buffer under the sink. The sink only copies/queues, so this is
     * short. */
    if (fn) {
        fn(s_out, out.length, keyframe, pts_ms, ctx);
    }
    xSemaphoreGive(s_lock);
}

uint32_t capture_h264_enc_us(void) { return s_enc_us; }
uint32_t capture_h264_frame_bytes(void) { return s_frame_bytes; }
uint32_t capture_h264_fps_x10(void) { return s_fps_x10; }

#else /* !CONFIG_P4KVM_WEBRTC_ENABLE */

void capture_h264_set_sink(capture_h264_sink_fn fn, void *ctx)
{
    (void)fn;
    (void)ctx;
}
void capture_h264_request_keyframe(void) {}
void capture_h264_on_csi_frame(const uint8_t *uyvy, capture_ctx_t *c)
{
    (void)uyvy;
    (void)c;
}
uint32_t capture_h264_enc_us(void) { return 0; }
uint32_t capture_h264_frame_bytes(void) { return 0; }
uint32_t capture_h264_fps_x10(void) { return 0; }

#endif
