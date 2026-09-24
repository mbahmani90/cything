#pragma once

/* Per-device Wi-Fi settings. */

/* Soft-AP the device runs while unpaired (see doc/pairing.md). The SSID is
 * built at boot as ESP_AP_WIFI_SSID_PREFIX + the last three bytes of the
 * chip's factory (eFuse) MAC in upper-case hex, e.g. "Cy_WiFi_15753C", so
 * every unit gets a unique name with no per-device configuration. Keep the
 * prefix <= 26 characters (SSID max is 32). */
#define ESP_AP_WIFI_SSID_PREFIX "Cy_WiFi_"
#define ESP_AP_WIFI_PASS        "123456789"

/* Pairing inactivity timeout: if the next step (ssid:/pass:/FinishP) does not
 * arrive within this long, the pairing state machine resets to StartP. */
#define PAIRING_TIMEOUT_MS      6000
