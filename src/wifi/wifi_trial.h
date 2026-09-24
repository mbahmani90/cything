#pragma once

#include <stddef.h>

/*
 * Try router credentials while the device is in pairing (soft-AP) mode,
 * without storing anything: switch to APSTA, connect the station, wait for
 * an IP. Used by BLE pairing on COMMIT so a wrong password is reported to
 * the phone instead of rebooting into a station mode that never connects
 * (doc/ble-pairing.md "Trying the credentials").
 *
 * On success the station stays up (the device reboots into station mode a
 * moment later anyway) and `device_ip` / the GET_INFO reply show the
 * router-assigned address. On failure the station is dropped and the device
 * goes back to plain AP mode, still pairable.
 */
typedef enum {
    WIFI_TRIAL_OK = 0,
    WIFI_TRIAL_NOT_FOUND,   /* no AP with that SSID in range */
    WIFI_TRIAL_AUTH_FAIL,   /* association/handshake refused — wrong password */
    WIFI_TRIAL_TIMEOUT,     /* no IP within timeout_ms */
    WIFI_TRIAL_ERROR,       /* esp_wifi call failed */
} wifi_trial_result_t;

/* Blocking; call from an ordinary task (not the NimBLE host task). */
wifi_trial_result_t wifi_trial_run(const char *ssid, const char *password, unsigned timeout_ms);
