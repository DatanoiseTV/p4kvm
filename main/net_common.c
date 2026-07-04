/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "net_common.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "p4kvm";

esp_err_t net_common_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

void net_mdns_init(void)
{
    /* Interface-agnostic: announces on every netif (Ethernet and/or WiFi). */
    esp_err_t mdns_err = mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
        return;
    }
    mdns_err = mdns_hostname_set(CONFIG_P4KVM_MDNS_HOSTNAME);
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname: %s", esp_err_to_name(mdns_err));
    }
    mdns_err = mdns_instance_name_set("P4KVM");
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance: %s", esp_err_to_name(mdns_err));
    }
    mdns_txt_item_t http_txt[] = {
        {"path", "/"},
    };
    mdns_err = mdns_service_add("P4KVM", "_http", "_tcp", 80, http_txt, 1);
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _http._tcp: %s", esp_err_to_name(mdns_err));
    } else {
        ESP_LOGI(TAG, "mDNS: http://" CONFIG_P4KVM_MDNS_HOSTNAME ".local/");
    }
}
