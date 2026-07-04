/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture_priv.h"

#include <inttypes.h>

#include "p4kvm_hw_defaults.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_SPIRAM
#error "Enable CONFIG_SPIRAM (PSRAM): 1080p frame buffers need external RAM. See sdkconfig.defaults."
#endif

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_csi_types.h"
#include "soc/clk_tree_defs.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_bridge_struct.h"

#include "tc358743.h"
#include "tc358743_hdmi_debug.h"

static esp_cam_ctlr_handle_t s_cam;
static isp_proc_handle_t s_isp_bypass;

static const uint32_t s_csi_expected_dt = CAPTURE_CSI_DATA_TYPE;

static capture_ctx_t s_cap;

static void tc358743_resetn_pulse(void)
{
#if CONFIG_P4KVM_TC358743_RST_GPIO >= 0
    const int rst = CONFIG_P4KVM_TC358743_RST_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(CAPTURE_LOG_TAG, "TC358743 RESETN released on GPIO %d", rst);
#else
    ESP_LOGW(CAPTURE_LOG_TAG, "TC358743 RESETN not wired - waiting 500 ms for internal POR");
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
}

static void wait_tc358743_pixel_stream(tc358743_t *tc, uint32_t timeout_ms)
{
    const uint32_t step = 50;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        uint8_t st = 0;
        if (tc358743_sys_status(tc, &st) == ESP_OK && (st & 0x02) != 0 && (st & 0x80) != 0) {
            ESP_LOGI(CAPTURE_LOG_TAG, "HDMI ready SYS_STATUS=0x%02x after %u ms", st, waited);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "HDMI lock wait %u ms - starting CSI anyway", (unsigned)timeout_ms);
}

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes)
{
    const uint32_t gdma_64b = (uint32_t)(c->hres * c->vres * bpp / 64);
    uint32_t brg_fc = MIPI_CSI_BRIDGE.frame_cfg.val;
    uint32_t isp_fc = ISP.frame_cfg.val;
    unsigned brg_has_hsync = (unsigned)((brg_fc >> 24) & 1u);
    unsigned brg_vcheck = (unsigned)((brg_fc >> 25) & 1u);
    unsigned isp_ls = (unsigned)((isp_fc >> 29) & 1u);
    unsigned isp_le = (unsigned)((isp_fc >> 30) & 1u);
    ESP_LOGW(CAPTURE_LOG_TAG,
             "CSI stall: fb=%zu B expect, GDMA size=%" PRIu32 "×64b for %ux%u@%ubpp | get_new=%" PRIu32 " done=%" PRIu32
             " ping=%d done_fb=%p",
             fb_bytes, gdma_64b, (unsigned)c->hres, (unsigned)c->vres, bpp, c->csi_get_new_irqs,
             c->csi_dma_done_irqs, c->ping_fb_idx, (void *)c->done_fb);
    ESP_LOGW(CAPTURE_LOG_TAG, "  esp_cam: csi_transfer_size=%" PRIu32 "×64b (=h×v×in_bpp/64); RGB888 in_bpp=24, wire datatype must match", gdma_64b);
    {
        uint32_t dtc = MIPI_CSI_BRIDGE.data_type_cfg.val;
        unsigned lo = (unsigned)(dtc & 0x3fu);
        unsigned hi = (unsigned)((dtc >> 8) & 0x3fu);
        ESP_LOGW(CAPTURE_LOG_TAG, "  BRG data_type filter min=0x%02x max=0x%02x (CSI-2 user: RGB888=0x24 YUV422_8b=0x1E …)", lo, hi);
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG frame_cfg=0x%08" PRIx32 " (vadr=%" PRIu32 " hadr=%" PRIu32 ") host_ctrl=0x%08" PRIx32
             " dtype_reg=0x%08" PRIx32,
             brg_fc, brg_fc & 0xfffu, (brg_fc >> 12) & 0xfffu, MIPI_CSI_BRIDGE.host_ctrl.val, MIPI_CSI_BRIDGE.data_type_cfg.val);
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG line packets: has_hsync=%u vadr_check=%u | ISP line packets: start=%u end=%u"
                            " (bridge counts exact rows/pixels, ISP counts row-1/pixel-1)",
             brg_has_hsync, brg_vcheck, isp_ls, isp_le);
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG csi_en=0x%08" PRIx32 " buf_flow=0x%08" PRIx32, MIPI_CSI_BRIDGE.csi_en.val,
             MIPI_CSI_BRIDGE.buf_flow_ctl.val);
    ESP_LOGW(CAPTURE_LOG_TAG, "  ISP frame_cfg=0x%08" PRIx32 " (vadr=%" PRIu32 " hadr=%" PRIu32 ") cntl=0x%08" PRIx32, isp_fc,
             isp_fc & 0xfffu, (isp_fc >> 12) & 0xfffu, ISP.cntl.val);
    {
        uint32_t ir = MIPI_CSI_BRIDGE.int_raw.val;
        uint32_t ist = MIPI_CSI_BRIDGE.int_st.val;
        uint32_t iena = MIPI_CSI_BRIDGE.int_ena.val;
        uint32_t m = ist & 0x3fu;
        ESP_LOGW(CAPTURE_LOG_TAG, "  BRG int raw=0x%08" PRIx32 " st=0x%08" PRIx32 " ena=0x%08" PRIx32
                                 " | st: vadr_gt:%u vadr_lt:%u discard:%u overrun:%u fifo_ovf:%u dma_upd:%u",
                 ir, ist, iena, (unsigned)(m >> 0) & 1u, (unsigned)(m >> 1) & 1u, (unsigned)(m >> 2) & 1u, (unsigned)(m >> 3) & 1u,
                 (unsigned)(m >> 4) & 1u, (unsigned)(m >> 5) & 1u);
    }
    {
        uint32_t bfc = MIPI_CSI_BRIDGE.buf_flow_ctl.val;
        uint32_t drc = MIPI_CSI_BRIDGE.dma_req_cfg.val;
        ESP_LOGW(
            CAPTURE_LOG_TAG, "  BRG endian=0x%08" PRIx32 " dmablk_size=%" PRIu32 " dma_burst_len=%" PRIu32
                            " dma_upd_by_blk:%u buf_depth=%" PRIu32 " afull_th=%" PRIu32 " dma_interval=%" PRIu32,
            MIPI_CSI_BRIDGE.endian_mode.val, (uint32_t)MIPI_CSI_BRIDGE.dmablk_size.dmablk_size, drc & 0xfffu,
            (unsigned)((drc >> 12) & 1u), (bfc >> 16) & 0x3fffu, bfc & 0x3fffu,
            (uint32_t)MIPI_CSI_BRIDGE.dma_req_interval.dma_req_interval);
    }
}

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp)
{
    /* Same in/out type keeps the CSI bridge color converter in bypass (the
     * converter itself is rev >= 3.0-only). For YUV422, UYVY lands in DRAM
     * as-is and the BitScrambler pass reorders it for the JPEG encoder. */
    csi->input_data_color_type = CAPTURE_CAM_COLOR;
    csi->output_data_color_type = CAPTURE_CAM_COLOR;
    isp->input_data_color_type = CAPTURE_ISP_COLOR;
    isp->output_data_color_type = CAPTURE_ISP_COLOR;
}

static void capture_configure_p4_csi_bridge(uint32_t hres, uint32_t vres)
{
    MIPI_CSI_BRIDGE.frame_cfg.hadr_num = hres;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num = vres;
    MIPI_CSI_BRIDGE.frame_cfg.has_hsync_e = 0u;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num_check = 0u;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3fu;
}

static bool IRAM_ATTR cam_on_get_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_get_new_irqs, 1);
    int i = c->ping_fb_idx % CAPTURE_FB_COUNT;
    trans->buffer = c->fb[i];
    trans->buflen = c->frame_bytes;
    c->ping_fb_idx = (c->ping_fb_idx + 1) % CAPTURE_FB_COUNT;
    return false;
}

static bool IRAM_ATTR cam_on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_dma_done_irqs, 1);
    if (trans && trans->buffer) {
        c->done_fb = trans->buffer;
    }
    BaseType_t high_task_woken = pdFALSE;
    if (c->csi_done_sem) {
        (void)xSemaphoreGiveFromISR(c->csi_done_sem, &high_task_woken);
    }
    return high_task_woken;
}

capture_ctx_t *capture_hw_init_start(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = P4KVM_MIPI_LDO_CHAN_ID,
        .voltage_mv = P4KVM_MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    tc358743_resetn_pulse();

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = P4KVM_TC358743_I2C_SDA_GPIO,
        .scl_io_num = P4KVM_TC358743_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
    ESP_ERROR_CHECK(tc358743_probe(i2c_bus, NULL, &s_cap.tc));
#if CONFIG_P4KVM_PIPELINE_YUV422_BS
    tc358743_select_csi_uyvy422(s_cap.tc, true);
#endif
    ESP_ERROR_CHECK(tc358743_init_streaming(s_cap.tc));

    s_cap.hres = P4KVM_CSI_H_RES;
    s_cap.vres = P4KVM_CSI_V_RES;
    s_cap.frame_bytes = (size_t)s_cap.hres * (size_t)s_cap.vres * (CAPTURE_PIXEL_BPP / 8u);

    size_t align = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align));
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *blk = heap_caps_aligned_calloc(align, CAPTURE_FB_COUNT, s_cap.frame_bytes, caps);
    if (!blk) {
        ESP_LOGE(CAPTURE_LOG_TAG, "CSI frame buffer alloc failed (%u×%zu bytes). Enable PSRAM or reduce capture resolution.",
                 CAPTURE_FB_COUNT, s_cap.frame_bytes);
        vTaskDelete(NULL);
        return NULL;
    }
    for (int i = 0; i < CAPTURE_FB_COUNT; i++) {
        s_cap.fb[i] = blk + ((size_t)i * s_cap.frame_bytes);
    }
    /*
     * calloc's zero-fill left the whole ring dirty in the write-back cache.
     * Flush and drop those lines once, now; afterwards no CPU write ever
     * touches the ring (CSI DMA writes, JPEG/BitScrambler DMA reads), so the
     * previous 6 MB-per-frame msync in the encode loop is unnecessary.
     */
    ESP_ERROR_CHECK(esp_cache_msync(blk, (size_t)CAPTURE_FB_COUNT * s_cap.frame_bytes,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
    s_cap.ping_fb_idx = 0;
    s_cap.done_fb = NULL;
    s_cap.csi_dma_done_irqs = 0;
    s_cap.csi_get_new_irqs = 0;

    ESP_LOGI(CAPTURE_LOG_TAG, "CSI %ubpp ring %u×%zu bytes (align %zu, DT 0x%02x)", CAPTURE_PIXEL_BPP, CAPTURE_FB_COUNT,
             s_cap.frame_bytes, align, (unsigned)s_csi_expected_dt);

    s_cap.csi_done_sem = xSemaphoreCreateCounting(32, 0);
    if (!s_cap.csi_done_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "CSI done sem");
        vTaskDelete(NULL);
        return NULL;
    }

    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = s_cap.hres,
        .v_res = s_cap.vres,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = P4KVM_MIPI_LANE_MBPS,
        .queue_items = CAPTURE_FB_COUNT,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80 * 1000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .yuv_range = ISP_COLOR_RANGE_LIMIT,
        .yuv_std = ISP_YUV_CONV_STD_BT709,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = s_cap.hres,
        .v_res = s_cap.vres,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .intr_priority = 0,
        .flags = {.bypass_isp = true, .byte_swap_en = false},
    };
    capture_fill_esp_cam_color_types(&csi_cfg, &isp_cfg);

    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam));

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = cam_on_get_new,
        .on_trans_finished = cam_on_done,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, &s_cap));

    ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam));
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_cfg, &s_isp_bypass));
    ISP.cntl.isp_en = 0;
    capture_configure_p4_csi_bridge(s_cap.hres, s_cap.vres);
   
    ESP_ERROR_CHECK(tc358743_enable_hdmi_output(s_cap.tc));
    wait_tc358743_pixel_stream(s_cap.tc, 5000);

    capture_configure_p4_csi_bridge(s_cap.hres, s_cap.vres);
    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam));
    ESP_LOGI(CAPTURE_LOG_TAG, "esp_cam_ctlr_start after HDMI lock");

    return &s_cap;
}

static void capture_drain_csi_done_sem(SemaphoreHandle_t sem)
{
    if (!sem) {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c, bool deep)
{
    ESP_RETURN_ON_FALSE(c && c->tc && c->csi_done_sem, ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "ctx");
    ESP_RETURN_ON_FALSE(s_cam, ESP_ERR_INVALID_STATE, CAPTURE_LOG_TAG, "cam");

    ESP_LOGW(CAPTURE_LOG_TAG, "HDMI/camera recover (%s): esp_cam stop → %s → MIPI reapply → esp_cam start",
             deep ? "deep" : "hotplug", deep ? "TC358743 full re-init" : "HDMI hotplug");

    esp_err_t er = esp_cam_ctlr_stop(s_cam);
    if (er == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(CAPTURE_LOG_TAG, "esp_cam_ctlr_stop: not started (%s), continuing", esp_err_to_name(er));
    } else if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "esp_cam_ctlr_stop: %s", esp_err_to_name(er));
        return er;
    }

    capture_drain_csi_done_sem(c->csi_done_sem);

    er = deep ? tc358743_full_reinit(c->tc) : tc358743_hdmi_hotplug_reset(c->tc);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "%s: %s", deep ? "tc358743_full_reinit" : "tc358743_hdmi_hotplug_reset",
                 esp_err_to_name(er));
    }

    wait_tc358743_pixel_stream(c->tc, 5000);

    er = tc358743_reapply_csi_path_after_hdmi(c->tc);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "tc358743_reapply_csi_path_after_hdmi: %s", esp_err_to_name(er));
    }

    capture_configure_p4_csi_bridge(c->hres, c->vres);

    c->ping_fb_idx = 0;
    c->done_fb = NULL;
    c->csi_dma_done_irqs = 0;
    c->csi_get_new_irqs = 0;

    er = esp_cam_ctlr_start(s_cam);
    if (er != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "esp_cam_ctlr_start after recover: %s", esp_err_to_name(er));
        return er;
    }
    ESP_LOGI(CAPTURE_LOG_TAG, "esp_cam_ctlr_start after HDMI recover");
    return ESP_OK;
}