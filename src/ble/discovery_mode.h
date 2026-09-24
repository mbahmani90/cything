#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Device discovery method control (doc/ble-scan-beacon.md, DISCOVERYMODE command). */

typedef enum {
    DISCOVERY_WIFI = 1,   /* Wi-Fi multicast scan only */
    DISCOVERY_BLE = 2,    /* BLE beacon only */
    DISCOVERY_BOTH = 3,   /* Both methods (default) */
} discovery_mode_t;

/* Initialize discovery mode storage from NVS. Called once at startup. */
void discovery_mode_init(void);

/* Get the current discovery mode; defaults to DISCOVERY_BOTH if not set. */
discovery_mode_t discovery_mode_get(void);

/* Set and persist the discovery mode to NVS. Returns ESP_OK on success. */
esp_err_t discovery_mode_set(discovery_mode_t mode);

/* Check if BLE beacon should be advertised for the current mode. */
bool discovery_mode_should_advertise_ble(void);

/* Check if Wi-Fi multicast scan should be used (app-side, informational). */
bool discovery_mode_should_scan_wifi(void);
