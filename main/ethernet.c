/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ethernet.h"

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

static const char *TAG = "p4kvm";

#if CONFIG_P4KVM_ETH_ENABLE
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;

static void eth_on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "Open http://" IPSTR "/ or http://" CONFIG_P4KVM_MDNS_HOSTNAME ".local/",
             IP2STR(&e->ip_info.ip));
}

esp_err_t ethernet_init(void)
{
    /* esp_netif / event loop: net_common_init() (called first from app_main). */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_P4KVM_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_P4KVM_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = CONFIG_P4KVM_ETH_RMII_CLK_GPIO;
#if SOC_EMAC_USE_MULTI_IO_MUX
    emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_P4KVM_ETH_RMII_TX_EN_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_P4KVM_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_P4KVM_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_P4KVM_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_P4KVM_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_P4KVM_ETH_RMII_RXD1_GPIO;
#endif
    emac_config.smi_gpio.mdc_num = CONFIG_P4KVM_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_P4KVM_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "eth mac");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "eth phy");
        return ESP_FAIL;
    }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "eth install");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_FAIL, TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_eth_netif, CONFIG_P4KVM_MDNS_HOSTNAME), TAG, "hostname");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "glue");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_on_got_ip, NULL),
                        TAG, "ip ev");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start");
    return ESP_OK;
}
#else
esp_err_t ethernet_init(void)
{
    ESP_LOGW(TAG, "Ethernet disabled (enable P4KVM_ETH_ENABLE or P4KVM_WIFI_ENABLE for HTTP)");
    return ESP_OK;
}
#endif
