/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi station link. The ESP32-P4 has no radio; on boards like the
 * ESP32-P4-nano the onboard ESP32-C6 provides WiFi over SDIO through the
 * esp_wifi_remote + esp_hosted components, so the standard esp_wifi API below
 * is transparently proxied to the C6. SDIO pinning/transport is configured by
 * esp_hosted's own Kconfig (defaults match the Espressif P4 reference design).
 */
#include "wifi_net.h"

#include "runtime_cfg.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "p4kvm_wifi";

#if CONFIG_P4KVM_WIFI_ENABLE

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

/* Reconnect backoff: fast first (transient AP hiccup), capped so a long AP
 * outage doesn't hammer the radio. Reset on every successful IP. */
#define WIFI_RECONNECT_MIN_MS 500
#define WIFI_RECONNECT_MAX_MS 8000

static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = WIFI_RECONNECT_MIN_MS;
static uint32_t s_disconnects;

static void wifi_reconnect_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void wifi_schedule_reconnect(void)
{
    if (!s_reconnect_timer) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_delay_ms * 1000ull);
    s_reconnect_delay_ms *= 2;
    if (s_reconnect_delay_ms > WIFI_RECONNECT_MAX_MS) {
        s_reconnect_delay_ms = WIFI_RECONNECT_MAX_MS;
    }
}

static void wifi_on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_disconnects++;
        ESP_LOGW(TAG, "disconnected (reason %d, #%lu), retry in %lu ms", e ? e->reason : -1,
                 (unsigned long)s_disconnects, (unsigned long)s_reconnect_delay_ms);
        /* Never block the event loop task; the one-shot timer reconnects. */
        wifi_schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_reconnect_delay_ms = WIFI_RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "Got IP: " IPSTR " - open http://" IPSTR "/ or http://" CONFIG_P4KVM_MDNS_HOSTNAME ".local/",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
        /* Power save costs tens of ms of input latency per packet; this is a
         * KVM. Re-assert after every association: some coprocessor firmwares
         * reset the mode on reconnect. */
        esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ps none: %s", esp_err_to_name(err));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost IP");
    }
}

esp_err_t wifi_net_init(void)
{
    char ssid[33];
    char pass[65];
    runtime_cfg_get_str(RT_KEY_WIFI_SSID, CONFIG_P4KVM_WIFI_SSID, ssid, sizeof(ssid));
    runtime_cfg_get_str(RT_KEY_WIFI_PASS, CONFIG_P4KVM_WIFI_PASSWORD, pass, sizeof(pass));
    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "WiFi support built but no SSID configured (web UI Setup or menuconfig) - skipping");
        return ESP_OK;
    }

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        ESP_LOGE(TAG, "wifi sta netif");
        return ESP_FAIL;
    }
    esp_err_t err = esp_netif_set_hostname(sta, CONFIG_P4KVM_MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hostname: %s", esp_err_to_name(err));
    }

    esp_timer_create_args_t targs = {
        .callback = wifi_reconnect_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_reconnect_timer), TAG, "reconnect timer");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init (C6 SDIO link up?)");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_on_event, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_on_event, NULL), TAG, "ip ev");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_on_event, NULL), TAG, "lost ip ev");

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    /* Reliability over first-association speed: scan every channel and pick
     * the strongest BSS instead of the first match (matters with repeaters /
     * mesh APs broadcasting the same SSID). */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt = 3; /* in-supplicant quick retries before a DISCONNECTED event */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ps none: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "WiFi STA connecting to \"%s\" (power save off)", ssid);
    return ESP_OK;
}

#else /* !CONFIG_P4KVM_WIFI_ENABLE */

esp_err_t wifi_net_init(void)
{
    ESP_LOGD(TAG, "WiFi disabled");
    return ESP_OK;
}

#endif
