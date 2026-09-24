#include "discovery_mode.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "common/cy_log.h"

#define NVS_NS           "cy_ble"
#define NVS_KEY_MODE     "disc_mode"

static SemaphoreHandle_t s_mutex = NULL;
static discovery_mode_t s_mode = DISCOVERY_BOTH;  /* Default: both methods */

static void mutex_init(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void discovery_mode_init(void) {
    mutex_init();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* Namespace doesn't exist yet, use default */
        CY_LOGI("DiscMode", "No saved discovery mode, using default: BOTH");
        return;
    }

    uint8_t mode = DISCOVERY_BOTH;
    if (nvs_get_u8(h, NVS_KEY_MODE, &mode) == ESP_OK) {
        if (mode >= DISCOVERY_WIFI && mode <= DISCOVERY_BOTH) {
            s_mode = (discovery_mode_t)mode;
            CY_LOGI("DiscMode", "Loaded discovery mode from NVS: %d", s_mode);
        } else {
            CY_LOGW("DiscMode", "Invalid mode in NVS: %d, using default", mode);
        }
    }

    nvs_close(h);
}

discovery_mode_t discovery_mode_get(void) {
    mutex_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    discovery_mode_t mode = s_mode;
    xSemaphoreGive(s_mutex);
    return mode;
}

esp_err_t discovery_mode_set(discovery_mode_t mode) {
    if (mode < DISCOVERY_WIFI || mode > DISCOVERY_BOTH) {
        return ESP_ERR_INVALID_ARG;
    }

    mutex_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        CY_LOGW("DiscMode", "Failed to open NVS: %d", err);
        return err;
    }

    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
        if (err == ESP_OK) {
            s_mode = mode;
            CY_LOGI("DiscMode", "Discovery mode set to: %d", mode);
        }
    }

    nvs_close(h);
    xSemaphoreGive(s_mutex);
    return err;
}

bool discovery_mode_should_advertise_ble(void) {
    discovery_mode_t mode = discovery_mode_get();
    return mode == DISCOVERY_BLE || mode == DISCOVERY_BOTH;
}

bool discovery_mode_should_scan_wifi(void) {
    discovery_mode_t mode = discovery_mode_get();
    return mode == DISCOVERY_WIFI || mode == DISCOVERY_BOTH;
}
