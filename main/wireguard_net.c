/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * WireGuard client tunnel (trombik/esp_wireguard - the lwIP implementation
 * derived from smartalock/wireguard-lwip, the same core as ciniml's
 * WireGuard-ESP32-Arduino). The KVM stays reachable through the tunnel from
 * anywhere the peer routes; combined with auth this is the intended remote
 * access path instead of exposing port 80.
 */
#include "wireguard_net.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "p4kvm_wg";

#if CONFIG_P4KVM_WG_ENABLE

#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Handshakes expire after 180 s; if the peer stays down this long after a
 * successful connect, tear down and re-handshake (endpoint IP may have
 * changed, NAT mapping may have died despite keepalive). */
#define WG_STALE_SECONDS 30
#define WG_POLL_MS 1000

typedef enum {
    WG_ST_OFF,
    WG_ST_STARTING,
    WG_ST_HANDSHAKING,
    WG_ST_UP,
    WG_ST_RETRYING,
} wg_state_t;

static volatile wg_state_t s_state = WG_ST_OFF;

static wireguard_config_t s_cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t s_ctx = {0};

const char *wireguard_net_status_str(void)
{
    switch (s_state) {
    case WG_ST_STARTING: return "starting";
    case WG_ST_HANDSHAKING: return "handshaking";
    case WG_ST_UP: return "up";
    case WG_ST_RETRYING: return "retrying";
    default: return "off";
    }
}

static bool wg_have_ip(void)
{
    const char *keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(keys[i]);
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            return true;
        }
    }
    return false;
}

static void wg_task(void *arg)
{
    (void)arg;
    s_state = WG_ST_STARTING;

    while (!wg_have_ip()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* WireGuard handshake timestamps (TAI64N) need sane wall-clock time. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
    } else {
        while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
            ESP_LOGW(TAG, "waiting for NTP time (handshake needs it)");
        }
        ESP_LOGI(TAG, "time synced");
    }

    for (;;) {
        s_state = WG_ST_HANDSHAKING;
        err = esp_wireguard_init(&s_cfg, &s_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wireguard_init: %s (check key/endpoint config)", esp_err_to_name(err));
            s_state = WG_ST_RETRYING;
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        err = esp_wireguard_connect(&s_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wireguard_connect: %s (DNS/endpoint down?)", esp_err_to_name(err));
            s_state = WG_ST_RETRYING;
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        int down_s = 0;
        bool was_up = false;
        while (down_s < WG_STALE_SECONDS) {
            vTaskDelay(pdMS_TO_TICKS(WG_POLL_MS));
            if (esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK) {
                if (!was_up) {
                    ESP_LOGI(TAG, "peer up (%s:%d)", s_cfg.endpoint, s_cfg.port);
                    was_up = true;
                }
                s_state = WG_ST_UP;
                down_s = 0;
            } else {
                if (was_up) {
                    ESP_LOGW(TAG, "peer down");
                    was_up = false;
                    s_state = WG_ST_HANDSHAKING;
                }
                down_s++;
            }
        }
        ESP_LOGW(TAG, "handshake stale for %d s - reconnecting", WG_STALE_SECONDS);
        s_state = WG_ST_RETRYING;
        esp_wireguard_disconnect(&s_ctx);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t wireguard_net_start(void)
{
    if (strlen(CONFIG_P4KVM_WG_PRIVATE_KEY) == 0 || strlen(CONFIG_P4KVM_WG_PEER_PUBLIC_KEY) == 0 ||
        strlen(CONFIG_P4KVM_WG_ENDPOINT) == 0) {
        ESP_LOGW(TAG, "WireGuard enabled but keys/endpoint not configured - skipping");
        return ESP_OK;
    }

    s_cfg.private_key = CONFIG_P4KVM_WG_PRIVATE_KEY;
    s_cfg.public_key = CONFIG_P4KVM_WG_PEER_PUBLIC_KEY;
#if defined(CONFIG_P4KVM_WG_PRESHARED_KEY)
    if (strlen(CONFIG_P4KVM_WG_PRESHARED_KEY) > 0) {
        s_cfg.preshared_key = CONFIG_P4KVM_WG_PRESHARED_KEY;
    }
#endif
    s_cfg.allowed_ip = CONFIG_P4KVM_WG_LOCAL_IP;
    s_cfg.allowed_ip_mask = CONFIG_P4KVM_WG_LOCAL_NETMASK;
    s_cfg.endpoint = CONFIG_P4KVM_WG_ENDPOINT;
    s_cfg.port = CONFIG_P4KVM_WG_PORT;
    s_cfg.persistent_keepalive = CONFIG_P4KVM_WG_KEEPALIVE;

    BaseType_t ok = xTaskCreate(wg_task, "wireguard", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#else /* !CONFIG_P4KVM_WG_ENABLE */

const char *wireguard_net_status_str(void)
{
    return "off";
}

esp_err_t wireguard_net_start(void)
{
    ESP_LOGD(TAG, "WireGuard disabled");
    return ESP_OK;
}

#endif
