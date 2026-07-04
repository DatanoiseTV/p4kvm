/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture_priv.h"

#include "sdkconfig.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "jpeg_frame.h"
#include "tc358743_hdmi_debug.h"
#include "video_stats.h"

#if CAPTURE_NEEDS_REORDER
#include "driver/bitscrambler_loopback.h"

/* Assembled from uyvy_to_yvyu.bsasm (see main/CMakeLists.txt). */
BITSCRAMBLER_PROGRAM(s_bs_prog_uyvy_to_yvyu, "uyvy_to_yvyu");
#endif

/* TC358743 SYS_STATUS bits (Linux tc358743_regs.h). */
#define SYS_STATUS_DDC5V 0x01u
#define SYS_STATUS_TMDS 0x02u
#define SYS_STATUS_SYNC 0x80u

/* Hotplug-cycle recoveries before escalating to a full TC358743 re-init. */
#define HDMI_RECOVER_HOTPLUG_ATTEMPTS 3
#define HDMI_RECOVER_COOLDOWN_US ((int64_t)8 * 1000000)
#define HDMI_RECOVER_DEEP_COOLDOWN_US ((int64_t)30 * 1000000)
/* With DDC5V=0 (source looks unplugged) still try occasionally: the bit can
 * read low transiently (chip mid-reset, marginal sources) and never trying
 * again would leave a live source black forever. */
#define HDMI_RECOVER_UNPLUGGED_COOLDOWN_US ((int64_t)60 * 1000000)

#define STATS_WINDOW_US ((int64_t)2 * 1000000)

static jpeg_encoder_handle_t s_jpeg_enc;

volatile video_stats_t g_video_stats;

const char *video_stats_pipeline_name(void)
{
#if CONFIG_P4KVM_TEST_PATTERN
    return CAPTURE_PIPELINE_NAME "+testpat";
#else
    return CAPTURE_PIPELINE_NAME;
#endif
}

/** Per-window accumulators, folded into g_video_stats every STATS_WINDOW_US. */
typedef struct {
    int64_t window_start_us;
    uint32_t cap_frames_at_start;
    uint32_t enc_frames;
    uint64_t enc_us_sum;
    uint64_t bs_us_sum;
    uint64_t jpeg_bytes_sum;
} stats_window_t;

static void stats_window_reset(stats_window_t *w, uint32_t cap_frames_now, int64_t now_us)
{
    w->window_start_us = now_us;
    w->cap_frames_at_start = cap_frames_now;
    w->enc_frames = 0;
    w->enc_us_sum = 0;
    w->bs_us_sum = 0;
    w->jpeg_bytes_sum = 0;
}

static void stats_window_publish(stats_window_t *w, capture_ctx_t *c, int64_t now_us)
{
    int64_t span = now_us - w->window_start_us;
    if (span < STATS_WINDOW_US) {
        return;
    }
    uint32_t cap_now = c->csi_dma_done_irqs;
    uint32_t cap_delta = cap_now - w->cap_frames_at_start;
    g_video_stats.cap_frames = cap_now;
    g_video_stats.cap_fps_x10 = (uint32_t)((uint64_t)cap_delta * 10000000ull / (uint64_t)span);
    g_video_stats.enc_fps_x10 = (uint32_t)((uint64_t)w->enc_frames * 10000000ull / (uint64_t)span);
    if (w->enc_frames > 0) {
        g_video_stats.enc_us = (uint32_t)(w->enc_us_sum / w->enc_frames);
        g_video_stats.bs_us = (uint32_t)(w->bs_us_sum / w->enc_frames);
        g_video_stats.jpeg_bytes = (uint32_t)(w->jpeg_bytes_sum / w->enc_frames);
    }
    if (c->tc) {
        uint8_t st = 0;
        if (tc358743_sys_status(c->tc, &st) == ESP_OK) {
            g_video_stats.sys_status = st;
            g_video_stats.hdmi_locked =
                ((st & (SYS_STATUS_TMDS | SYS_STATUS_SYNC)) == (SYS_STATUS_TMDS | SYS_STATUS_SYNC));
        }
    } else {
        g_video_stats.hdmi_locked = 1; /* test pattern: source is always "up" */
    }
    stats_window_reset(w, cap_now, now_us);
}

/**
 * Frame-stall handling with escalation. Returns after (possibly) recovering;
 * the caller re-enters the semaphore wait.
 */
static void handle_csi_timeout(capture_ctx_t *c, unsigned bpp, int64_t *cooldown_until_us, int *fail_streak)
{
    if (!c->tc) {
        /* Test-pattern source has no bridge to recover; a stall here is a
         * software bug in the generator task. */
        ESP_LOGW(CAPTURE_LOG_TAG, "test-pattern frame wait timeout");
        return;
    }

    uint8_t st = 0;
    bool have_st = (tc358743_sys_status(c->tc, &st) == ESP_OK);
    if (have_st) {
        g_video_stats.sys_status = st;
        g_video_stats.hdmi_locked = 0;
    }

    if (have_st && (st & SYS_STATUS_DDC5V) == 0) {
        /* No +5 V from the source: cable unplugged or host fully off. Don't
         * hammer HPD, but do retry slowly - DDC5V can read low transiently
         * and never retrying would leave a live source black forever. */
        static uint32_t s_unplugged_logs;
        if ((s_unplugged_logs++ % 16u) == 0u) {
            ESP_LOGW(CAPTURE_LOG_TAG, "no HDMI source (SYS_STATUS=0x%02x, DDC5V=0) - waiting", st);
        }
        *fail_streak = 0;
        int64_t now = (int64_t)esp_timer_get_time();
        if (now >= *cooldown_until_us) {
            g_video_stats.recoveries++;
            (void)capture_hw_hdmi_recover(c, false);
            *cooldown_until_us = now + HDMI_RECOVER_UNPLUGGED_COOLDOWN_US;
        }
        return;
    }

    ESP_LOGW(CAPTURE_LOG_TAG, "csi frame wait timeout (dma_done_irqs=%lu, SYS_STATUS=0x%02x)",
             (unsigned long)c->csi_dma_done_irqs, st);
    capture_debug_csi_timeout(c, bpp, c->frame_bytes);
#if CONFIG_P4KVM_TC358743_ADV_DEBUG
    static uint32_t s_csi_timeout_logs;
    tc358743_debug_stall_extras(c->tc);
    if ((s_csi_timeout_logs++ % 8u) == 0u) {
        tc358743_debug_status(c->tc);
        tc358743_debug_bridge(c->tc);
    }
#endif

    int64_t now = (int64_t)esp_timer_get_time();
    if (now < *cooldown_until_us) {
        return;
    }
    bool deep = (*fail_streak >= HDMI_RECOVER_HOTPLUG_ATTEMPTS);
    (*fail_streak)++;
    g_video_stats.recoveries++;
    (void)capture_hw_hdmi_recover(c, deep);
    *cooldown_until_us = now + (deep ? HDMI_RECOVER_DEEP_COOLDOWN_US : HDMI_RECOVER_COOLDOWN_US);
}

void capture_mjpeg_run(capture_ctx_t *c)
{
    jpeg_encode_engine_cfg_t jcfg = {.intr_priority = 0, .timeout_ms = 120};
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jcfg, &s_jpeg_enc));

    /*
     * Worst-case JPEG at quality 100 can exceed 1 byte/pixel, and the YUV422
     * pipeline emits 4:2:2 scans (more chroma than 4:2:0). 1.5 B/px covers
     * both with margin; oversized encodes fail cleanly and drop the frame.
     */
    const size_t jpeg_cap = (size_t)c->hres * (size_t)c->vres * 3u / 2u;
    jpeg_encode_memory_alloc_cfg_t jmem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t smallest_alloc = SIZE_MAX;
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        size_t ja = 0;
        g_jpeg_frame.jpeg_buf[i] = jpeg_alloc_encoder_mem(jpeg_cap, &jmem, &ja);
        if (!g_jpeg_frame.jpeg_buf[i]) {
            ESP_LOGE(CAPTURE_LOG_TAG, "jpeg_alloc_encoder_mem failed slot %d", i);
            vTaskDelete(NULL);
            return;
        }
        if (ja < smallest_alloc) {
            smallest_alloc = ja;
        }
    }
    g_jpeg_frame.jpeg_cap = smallest_alloc;
    g_jpeg_frame.front_idx = -1;
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        g_jpeg_frame.jpeg_len[i] = 0;
        g_jpeg_frame.slot_ref[i] = 0;
    }
    g_jpeg_frame.frame_seq = 0;
    g_jpeg_frame.mutex = xSemaphoreCreateMutex();
    g_jpeg_frame.xmit_mutex = xSemaphoreCreateMutex();
    /* Enough tokens when several /stream clients are connected (see jpeg_frame_notify_new_frame). */
    g_jpeg_frame.frame_ready_sem = xSemaphoreCreateCounting(128, 0);
    if (!g_jpeg_frame.mutex || !g_jpeg_frame.xmit_mutex || !g_jpeg_frame.frame_ready_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "JPEG mutex alloc failed");
        vTaskDelete(NULL);
        return;
    }

#if CAPTURE_NEEDS_REORDER
    /*
     * BitScrambler loopback: DMA-reads UYVY from the CSI framebuffer, rotates
     * each 32-bit word one byte right, DMA-writes YVYU into s_yvyu_buf for
     * the JPEG encoder. bitscrambler_loopback_run() handles cache coherency
     * for both buffers on every run.
     */
    bitscrambler_handle_t bs = NULL;
    ESP_ERROR_CHECK(bitscrambler_loopback_create(&bs, SOC_BITSCRAMBLER_ATTACH_GPSPI2, c->frame_bytes));
    ESP_ERROR_CHECK(bitscrambler_load_program(bs, s_bs_prog_uyvy_to_yvyu));

    size_t align = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align));
    uint8_t *yvyu_buf =
        heap_caps_aligned_calloc(align, 1, c->frame_bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!yvyu_buf) {
        ESP_LOGE(CAPTURE_LOG_TAG, "YVYU reorder buffer alloc failed (%zu bytes)", c->frame_bytes);
        vTaskDelete(NULL);
        return;
    }
    /* Drop calloc's dirty zero-fill lines so later DMA writes can't be shadowed. */
    ESP_ERROR_CHECK(esp_cache_msync(yvyu_buf, c->frame_bytes,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
    ESP_LOGI(CAPTURE_LOG_TAG, "BitScrambler UYVY→YVYU reorder ready (%zu bytes/frame)", c->frame_bytes);
#endif

    const unsigned bpp = CAPTURE_PIXEL_BPP;
    int64_t hdmi_recover_cooldown_until_us = 0;
    int hdmi_recover_fail_streak = 0;

    stats_window_t win;
    stats_window_reset(&win, c->csi_dma_done_irqs, (int64_t)esp_timer_get_time());

    while (1) {
        if (xSemaphoreTake(c->csi_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            handle_csi_timeout(c, bpp, &hdmi_recover_cooldown_until_us, &hdmi_recover_fail_streak);
            continue;
        }
        hdmi_recover_cooldown_until_us = 0;
        hdmi_recover_fail_streak = 0;
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
            /* Drop stale completions; done_fb always points at the newest completed frame. */
        }

        void *src = (void *)c->done_fb;
        if (!src) {
            continue;
        }
        /*
         * No cache maintenance needed here: the CPU never touches the pixel
         * data. The ring was flushed+invalidated once after allocation, the
         * JPEG driver write-backs its input region itself, and the
         * BitScrambler loopback driver syncs both of its buffers per run.
         */

        uint8_t q = g_jpeg_frame.jpeg_quality;
        if (q < 1u) {
            q = 1u;
        } else if (q > 100u) {
            q = 100u;
        }
        jpeg_encode_cfg_t enc = {.width = c->hres,
                                 .height = c->vres,
                                 .src_type = CAPTURE_JPEG_SRC_TYPE,
                                 .sub_sample = CAPTURE_JPEG_SUBSAMPLE,
                                 .image_quality = q};
        const uint32_t jpeg_in_bytes = (uint32_t)c->frame_bytes;
        uint32_t out_sz = 0;
        int back = -1;
        for (;;) {
            if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            back = jpeg_frame_pick_encode_slot();
            if (back >= 0) {
                xSemaphoreGive(g_jpeg_frame.mutex);
                break;
            }
            xSemaphoreGive(g_jpeg_frame.mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        int64_t t0 = (int64_t)esp_timer_get_time();
        const uint8_t *enc_src = (const uint8_t *)src;
#if CAPTURE_NEEDS_REORDER
        size_t bs_written = 0;
        esp_err_t ber = bitscrambler_loopback_run(bs, src, c->frame_bytes, yvyu_buf, c->frame_bytes, &bs_written);
        if (ber != ESP_OK || bs_written != c->frame_bytes) {
            /* Throttled: at frame rate this would otherwise flood the UART. */
            static uint32_t s_bs_err_logs;
            g_video_stats.enc_errors++;
            if ((s_bs_err_logs++ % 64u) == 0u) {
                ESP_LOGW(CAPTURE_LOG_TAG, "bitscrambler %s (wrote %zu of %zu, %lu drops)", esp_err_to_name(ber),
                         bs_written, c->frame_bytes, (unsigned long)s_bs_err_logs);
            }
            continue;
        }
        enc_src = yvyu_buf;
#endif
        int64_t t1 = (int64_t)esp_timer_get_time();

        esp_err_t er = jpeg_encoder_process(s_jpeg_enc, &enc, enc_src, jpeg_in_bytes, g_jpeg_frame.jpeg_buf[back],
                                            (uint32_t)g_jpeg_frame.jpeg_cap, &out_sz);
        int64_t t2 = (int64_t)esp_timer_get_time();
        if (er != ESP_OK) {
            g_video_stats.enc_errors++;
            ESP_LOGW(CAPTURE_LOG_TAG, "jpeg %s", esp_err_to_name(er));
            continue;
        }
        if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
            g_jpeg_frame.jpeg_len[back] = (size_t)out_sz;
            g_jpeg_frame.front_idx = back;
            g_jpeg_frame.frame_seq++;
            xSemaphoreGive(g_jpeg_frame.mutex);
        }
        jpeg_frame_notify_new_frame();

        g_video_stats.enc_frames++;
        win.enc_frames++;
        win.bs_us_sum += (uint64_t)(t1 - t0);
        win.enc_us_sum += (uint64_t)(t2 - t1);
        win.jpeg_bytes_sum += out_sz;
        stats_window_publish(&win, c, t2);
    }
}
