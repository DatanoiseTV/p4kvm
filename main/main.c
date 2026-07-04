/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "atx_ctrl.h"
#include "capture.h"
#include "ethernet.h"
#include "http_server.h"
#include "jpeg_frame.h"
#include "net_common.h"
#include "usb_hid.h"
#include "wifi_net.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(net_common_init());
    ESP_ERROR_CHECK(ethernet_init());
    ESP_ERROR_CHECK(wifi_net_init());
    net_mdns_init();
    ESP_ERROR_CHECK(usb_hid_init());
    ESP_ERROR_CHECK(atx_ctrl_init());

    g_jpeg_frame.front_idx = -1;
    g_jpeg_frame.jpeg_quality = (uint8_t)CONFIG_P4KVM_JPEG_QUALITY;
    jpeg_quality_load_from_nvs();
    g_jpeg_frame.jpeg_buf[0] = NULL;
    g_jpeg_frame.jpeg_buf[1] = NULL;
    g_jpeg_frame.jpeg_buf[2] = NULL;
    g_jpeg_frame.xmit_mutex = NULL;
    (void)http_server_start();

    capture_start();
}
