#pragma once

#include "esp_err.h"

#define FLASH_WIFI_MODE_OFFSET        0x000
#define FLASH_ROUTER_SSID_OFFSET      0x100
#define FLASH_ROUTER_PASSWORD_OFFSET  0x400

#define FLASH_WIFI_MODE_SIZE          0x100
#define FLASH_ROUTER_SSID_SIZE        0x100
#define FLASH_ROUTER_PASSWORD_SIZE    0x100

extern char flash_wifi_mode      [FLASH_WIFI_MODE_SIZE      ];
extern char flash_router_ssid    [FLASH_ROUTER_SSID_SIZE    ];
extern char flash_router_password[FLASH_ROUTER_PASSWORD_SIZE];

esp_err_t flash_store_wifi_router_info(char *ssid , char *password , char *new_wifi_mode , int ssid_len, int password_len);
esp_err_t flash_read_wifi_router_info();