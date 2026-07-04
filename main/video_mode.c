/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "video_mode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "tc358743_edid_1080p30.h"
#include "tc358743_edid_720p60.h"

static const char *TAG = "p4kvm";
static const char *const k_nvs_ns = "p4kvm";
static const char *const k_nvs_key = "vmode";

typedef struct {
    const char *name;
    uint32_t hres;
    uint32_t vres;
    const uint8_t *edid;
    size_t edid_len;
} video_mode_desc_t;

static const video_mode_desc_t s_modes[] = {
    [VIDEO_MODE_720P60] = {"720p60", 1280, 720, tc358743_edid_720p60_bin, TC358743_EDID_720P60_TOTAL_LEN},
    [VIDEO_MODE_1080P30] = {"1080p30", 1920, 1080, tc358743_edid_bin, TC358743_EDID_TOTAL_LEN},
};
#define MODE_COUNT (sizeof(s_modes) / sizeof(s_modes[0]))

static video_mode_t s_mode = VIDEO_MODE_720P60;

void video_mode_init(void)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, k_nvs_key, &v);
    nvs_close(h);
    if (err == ESP_OK && v < MODE_COUNT) {
        s_mode = (video_mode_t)v;
    }
    ESP_LOGI(TAG, "video mode: %s (%lux%lu)", s_modes[s_mode].name, (unsigned long)s_modes[s_mode].hres,
             (unsigned long)s_modes[s_mode].vres);
}

video_mode_t video_mode_get(void)
{
    return s_mode;
}

const char *video_mode_name(void)
{
    return s_modes[s_mode].name;
}

uint32_t video_mode_hres(void)
{
    return s_modes[s_mode].hres;
}

uint32_t video_mode_vres(void)
{
    return s_modes[s_mode].vres;
}

const uint8_t *video_mode_edid(size_t *len)
{
    if (len) {
        *len = s_modes[s_mode].edid_len;
    }
    return s_modes[s_mode].edid;
}

static void video_mode_restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

esp_err_t video_mode_set_and_reboot(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < MODE_COUNT; i++) {
        if (strcmp(name, s_modes[i].name) != 0) {
            continue;
        }
        if ((video_mode_t)i == s_mode) {
            return ESP_OK; /* already active, no restart */
        }
        nvs_handle_t h;
        esp_err_t err = nvs_open(k_nvs_ns, NVS_READWRITE, &h);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_set_u8(h, k_nvs_key, (uint8_t)i);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "video mode -> %s, restarting", name);
        /* One-shot delay so the HTTP response reaches the client first. */
        static esp_timer_handle_t s_restart_timer;
        if (!s_restart_timer) {
            const esp_timer_create_args_t targs = {
                .callback = video_mode_restart_cb,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "vmode_rst",
            };
            if (esp_timer_create(&targs, &s_restart_timer) != ESP_OK) {
                esp_restart(); /* fallback: restart now */
            }
        }
        esp_timer_start_once(s_restart_timer, 400 * 1000);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
