#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"

#define  AP_MODE     1
#define  STA_MODE    2

#define WIFI_STA_MODE "WiMStA\r\n"
#define WIFI_AP_MODE  "WiMAcP\r\n"

/* IPv4 address of the active interface (AP or STA). Refreshed by
 * wifi_update_device_ip() when the soft-AP starts and on every
 * IP_EVENT_STA_GOT_IP; format with IPSTR / IP2STR(&device_ip). */
extern esp_ip4_addr_t device_ip;
void wifi_update_device_ip(esp_netif_t *p_netif);

/* The chip's factory base MAC from eFuse — unique per ESP32, read-only. Use
 * this (not a per-interface esp_wifi_get_mac()) as the stable device id. */
void wifi_get_base_mac(uint8_t mac[6]);

/* Per-unit suffix: the last 3 bytes of the base MAC as 6 upper-case hex
 * digits, e.g. "15753C". `out` must hold at least 7 bytes. The soft-AP SSID
 * and the BLE device name both end in it, so the app can match the two. */
void wifi_get_unit_suffix(char *out, size_t out_size);

/* Soft-AP SSID for this unit: ESP_AP_WIFI_SSID_PREFIX + wifi_get_unit_suffix().
 * `out_size` >= 32 is always enough. */
void wifi_get_ap_ssid(char *out, size_t out_size);
