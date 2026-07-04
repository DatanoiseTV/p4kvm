/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Animated test-pattern source: color bars with a sweeping stripe, generated
 * by the CPU into the same framebuffer ring the CSI DMA would fill. Publishes
 * frames through the identical done_fb/semaphore contract, so the entire
 * downstream pipeline (BitScrambler reorder, JPEG encode, HTTP stream, web UI,
 * stats) runs unmodified without the TC358743/CSI hardware attached.
 */
#include "capture_priv.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "p4kvm_hw_defaults.h"

#if CONFIG_P4KVM_TEST_PATTERN

#define TESTPAT_FPS 30
/* Rows of the animated band at the top of the frame (multiple of 8 for JPEG MCUs). */
#define TESTPAT_BAND_ROWS 96u
#define TESTPAT_STRIPE_PX 64u
#define TESTPAT_BAR_COUNT 8u

static capture_ctx_t s_cap;

typedef struct {
    uint8_t r, g, b;   /* RGB888 pipeline (stored B,G,R like the CSI ring) */
    uint8_t y, u, v;   /* YUV422 pipeline (BT.601 full-swing approximations) */
} testpat_color_t;

/* 100 % color bars: white, yellow, cyan, green, magenta, red, blue, black. */
static const testpat_color_t s_bars[TESTPAT_BAR_COUNT] = {
    {255, 255, 255, 235, 128, 128},
    {255, 255, 0, 210, 16, 146},
    {0, 255, 255, 170, 166, 16},
    {0, 255, 0, 145, 54, 34},
    {255, 0, 255, 106, 202, 222},
    {255, 0, 0, 81, 90, 240},
    {0, 0, 255, 41, 240, 110},
    {0, 0, 0, 16, 128, 128},
};

static uint8_t *s_row_template; /* one row of bars in the native pixel format */
static size_t s_row_bytes;

/** Write pixels [x0, x0+n) of a row buffer in the native format. n and x0 even for YUV. */
static void testpat_fill_span(uint8_t *row, uint32_t x0, uint32_t n, const testpat_color_t *c)
{
#if CONFIG_P4KVM_PIPELINE_YUV422_BS
    /* UYVY: 4 bytes per 2 pixels. */
    uint8_t *p = row + (size_t)(x0 / 2u) * 4u;
    for (uint32_t i = 0; i < n / 2u; i++) {
        p[0] = c->u;
        p[1] = c->y;
        p[2] = c->v;
        p[3] = c->y;
        p += 4;
    }
#else
    /* Matches the CSI ring's in-memory order (B, G, R ascending). */
    uint8_t *p = row + (size_t)x0 * 3u;
    for (uint32_t i = 0; i < n; i++) {
        p[0] = c->b;
        p[1] = c->g;
        p[2] = c->r;
        p += 3;
    }
#endif
}

static void testpat_render_template_row(uint32_t hres)
{
    uint32_t bar_w = hres / TESTPAT_BAR_COUNT;
    bar_w &= ~1u;
    for (uint32_t b = 0; b < TESTPAT_BAR_COUNT; b++) {
        uint32_t x0 = b * bar_w;
        uint32_t n = (b == TESTPAT_BAR_COUNT - 1u) ? (hres - x0) : bar_w;
        testpat_fill_span(s_row_template, x0, n & ~1u, &s_bars[b]);
    }
}

static void testpat_task(void *arg)
{
    (void)arg;
    capture_ctx_t *c = &s_cap;
    const size_t band_bytes = (size_t)TESTPAT_BAND_ROWS * s_row_bytes;
    uint32_t frame = 0;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / TESTPAT_FPS);

    while (1) {
        int i = c->ping_fb_idx % CAPTURE_FB_COUNT;
        uint8_t *fb = (uint8_t *)c->fb[i];

        /* Sweep a white stripe across the top band; the rest of the frame is
         * the static bars written once at init. */
        uint32_t sweep_range = c->hres - TESTPAT_STRIPE_PX;
        uint32_t x = (frame * 8u) % (2u * sweep_range);
        if (x > sweep_range) {
            x = 2u * sweep_range - x; /* bounce */
        }
        x &= ~1u;
        for (uint32_t r = 0; r < TESTPAT_BAND_ROWS; r++) {
            uint8_t *row = fb + (size_t)r * s_row_bytes;
            memcpy(row, s_row_template, s_row_bytes);
            testpat_fill_span(row, x, TESTPAT_STRIPE_PX, &s_bars[0]);
            /* 1-row black border above/below the stripe rows makes tearing visible. */
            if (r == 0 || r == TESTPAT_BAND_ROWS - 1u) {
                testpat_fill_span(row, 0, c->hres & ~1u, &s_bars[7]);
            }
        }
        /* CPU wrote the band: write it back so the encoder DMA sees it (see
         * the coherency note in capture_priv.h). */
        ESP_ERROR_CHECK(esp_cache_msync(fb, band_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M));

        c->done_fb = fb;
        c->ping_fb_idx = (i + 1) % CAPTURE_FB_COUNT;
        c->csi_dma_done_irqs++;
        frame++;
        xSemaphoreGive(c->csi_done_sem);

        vTaskDelayUntil(&last_wake, period);
    }
}

capture_ctx_t *capture_testpat_init_start(void)
{
    s_cap.hres = P4KVM_CSI_H_RES;
    s_cap.vres = P4KVM_CSI_V_RES;
    s_cap.frame_bytes = (size_t)s_cap.hres * (size_t)s_cap.vres * (CAPTURE_PIXEL_BPP / 8u);
    s_row_bytes = (size_t)s_cap.hres * (CAPTURE_PIXEL_BPP / 8u);

    size_t align = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align));
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *blk = heap_caps_aligned_calloc(align, CAPTURE_FB_COUNT, s_cap.frame_bytes, caps);
    s_row_template = heap_caps_malloc(s_row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blk || !s_row_template) {
        ESP_LOGE(CAPTURE_LOG_TAG, "test pattern alloc failed (%u×%zu + %zu bytes)", CAPTURE_FB_COUNT,
                 s_cap.frame_bytes, s_row_bytes);
        vTaskDelete(NULL);
        return NULL;
    }

    testpat_render_template_row(s_cap.hres);
    for (int i = 0; i < CAPTURE_FB_COUNT; i++) {
        s_cap.fb[i] = blk + ((size_t)i * s_cap.frame_bytes);
        for (uint32_t r = 0; r < s_cap.vres; r++) {
            memcpy((uint8_t *)s_cap.fb[i] + (size_t)r * s_row_bytes, s_row_template, s_row_bytes);
        }
    }
    /* Static rows are CPU-written once: flush the whole ring so encoder DMA
     * sees them; the animated band is re-flushed every frame. */
    ESP_ERROR_CHECK(esp_cache_msync(blk, (size_t)CAPTURE_FB_COUNT * s_cap.frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M));

    s_cap.tc = NULL;
    s_cap.ping_fb_idx = 0;
    s_cap.done_fb = NULL;
    s_cap.csi_dma_done_irqs = 0;
    s_cap.csi_get_new_irqs = 0;
    s_cap.csi_done_sem = xSemaphoreCreateCounting(32, 0);
    if (!s_cap.csi_done_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "test pattern sem alloc failed");
        vTaskDelete(NULL);
        return NULL;
    }

    ESP_LOGI(CAPTURE_LOG_TAG, "test pattern source: %ux%u %s @ %d fps (no TC358743)", (unsigned)s_cap.hres,
             (unsigned)s_cap.vres, CAPTURE_PIPELINE_NAME, TESTPAT_FPS);

    BaseType_t ok = xTaskCreatePinnedToCore(testpat_task, "testpat", 4096, NULL, 4, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(CAPTURE_LOG_TAG, "test pattern task create failed");
        vTaskDelete(NULL);
        return NULL;
    }
    return &s_cap;
}

#endif /* CONFIG_P4KVM_TEST_PATTERN */
