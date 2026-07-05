/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "jpeg_frame.h"

#include "esp_log.h"
#include "nvs.h"

#include "freertos/task.h"

jpeg_frame_slot_t g_jpeg_frame;

static const char *const k_nvs_ns = "p4kvm";
static const char *const k_nvs_key_jpeg_q = "jpeg_q";
static const char *const k_nvs_key_max_fps = "max_fps";

static portMUX_TYPE s_stream_mu = portMUX_INITIALIZER_UNLOCKED;
static int s_stream_clients;

void jpeg_frame_stream_enter(void)
{
    taskENTER_CRITICAL(&s_stream_mu);
    s_stream_clients++;
    taskEXIT_CRITICAL(&s_stream_mu);
}

void jpeg_frame_stream_leave(void)
{
    taskENTER_CRITICAL(&s_stream_mu);
    if (s_stream_clients > 0) {
        s_stream_clients--;
    }
    taskEXIT_CRITICAL(&s_stream_mu);
}

int jpeg_frame_stream_clients(void)
{
    int n;
    taskENTER_CRITICAL(&s_stream_mu);
    n = s_stream_clients;
    taskEXIT_CRITICAL(&s_stream_mu);
    return n;
}

void jpeg_frame_notify_new_frame(void)
{
    int n;
    taskENTER_CRITICAL(&s_stream_mu);
    n = s_stream_clients;
    taskEXIT_CRITICAL(&s_stream_mu);

    if (!g_jpeg_frame.frame_ready_sem || n <= 0) {
        return;
    }
    const int cap = 16;
    if (n > cap) {
        n = cap;
    }
    for (int i = 0; i < n; i++) {
        if (xSemaphoreGive(g_jpeg_frame.frame_ready_sem) != pdTRUE) {
            break;
        }
    }
}

int jpeg_frame_pick_encode_slot(void)
{
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        if (g_jpeg_frame.slot_ref[i] != 0) {
            continue;
        }
        if (g_jpeg_frame.front_idx >= 0 && i == g_jpeg_frame.front_idx) {
            continue;
        }
        return i;
    }
    return -1;
}

void jpeg_quality_load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(k_nvs_ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    uint8_t q = 0;
    err = nvs_get_u8(h, k_nvs_key_jpeg_q, &q);
    nvs_close(h);
    if (err != ESP_OK || q < 1u || q > 100u) {
        return;
    }
    g_jpeg_frame.jpeg_quality = q;
}

esp_err_t jpeg_quality_save_to_nvs(uint8_t q)
{
    if (q < 1u || q > 100u) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(k_nvs_ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW("p4kvm", "nvs_open %s: %s", k_nvs_ns, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, k_nvs_key_jpeg_q, q);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW("p4kvm", "jpeg_q NVS write: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

void stream_max_fps_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(k_nvs_ns, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t f = 0xff;
    esp_err_t err = nvs_get_u8(h, k_nvs_key_max_fps, &f);
    nvs_close(h);
    if (err != ESP_OK || f > 60u) {
        return;
    }
    g_jpeg_frame.stream_max_fps = f;
}

esp_err_t stream_max_fps_save_to_nvs(uint8_t fps)
{
    if (fps > 60u) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(k_nvs_ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW("p4kvm", "nvs_open %s: %s", k_nvs_ns, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, k_nvs_key_max_fps, fps);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW("p4kvm", "max_fps NVS write: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}
