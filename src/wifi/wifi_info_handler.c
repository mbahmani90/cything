#include "wifi_info_handler.h"

#include <stdio.h>
#include "esp_err.h"
#include "esp_mac.h"
#include "wifi/wifi_config.h"
#include "device_config/cy_config.h"

esp_ip4_addr_t device_ip = { 0 };

void wifi_update_device_ip(esp_netif_t *p_netif){
    esp_netif_ip_info_t if_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(p_netif, &if_info));
    device_ip = if_info.ip;
}

void wifi_get_base_mac(uint8_t mac[6]){
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
}

void wifi_get_unit_suffix(char *out, size_t out_size){
    uint8_t mac[6];
    wifi_get_base_mac(mac);
    snprintf(out, out_size, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void wifi_get_ap_ssid(char *out, size_t out_size){
    char suffix[7];
    wifi_get_unit_suffix(suffix, sizeof(suffix));
    snprintf(out, out_size, "%s%s", cy_ap_ssid_prefix, suffix);
}
