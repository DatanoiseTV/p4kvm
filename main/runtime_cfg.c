/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "runtime_cfg.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "p4kvm_cfg";
static const char *const k_ns = "p4kvmcfg";

void runtime_cfg_get_str(const char *key, const char *fallback, char *buf, size_t sz)
{
    if (!buf || sz == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(k_ns, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sz;
        esp_err_t err = nvs_get_str(h, key, buf, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            buf[sz - 1] = '\0';
            return;
        }
    }
    strlcpy(buf, fallback ? fallback : "", sz);
}

int32_t runtime_cfg_get_i32(const char *key, int32_t fallback)
{
    nvs_handle_t h;
    int32_t v = fallback;
    if (nvs_open(k_ns, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, key, &v) != ESP_OK) {
            v = fallback;
        }
        nvs_close(h);
    }
    return v;
}

static esp_err_t rt_open_rw(nvs_handle_t *h)
{
    esp_err_t err = nvs_open(k_ns, NVS_READWRITE, h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t runtime_cfg_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = rt_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t runtime_cfg_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    esp_err_t err = rt_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
