/*
 * Shared NimBLE bring-up — see ble_host.h. NimBLE rather than Bluedroid
 * because the arduino-esp32 precompiled Bluedroid is the full dual-mode host
 * and adds ~700 KB to the image (it no longer fits a 1.5 MB OTA slot);
 * NimBLE is ~250 KB and the same C API serves both builds: ESP-IDF's
 * built-in host, or the NimBLE-Arduino library (which bundles the same host
 * source) under Arduino/PlatformIO.
 */
#include "ble_host.h"

#include <string.h>

#include <inttypes.h>
#include "esp_system.h"
#include "ble/ble_nimble.h"
#include "common/cy_log.h"
#include "device_config/cy_config.h"

/* Declared in store/config/ble_store_config.h in ESP-IDF, nowhere public
 * in NimBLE-Arduino; the symbol exists in both. */
void ble_store_config_init(void);

int ble_host_init(void){
#ifdef ARDUINO
    /* Under Arduino nimble_port_init() leaves the controller to the caller
     * (NimBLE-Arduino does this in NimBLEDevice::init). */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    /* Dual-mode chip: BLE only, hand the Classic BT RAM back first (the core
     * has usually done that already), and the controller mode must match
     * esp_bt_controller_enable() below. */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    bt_cfg.mode = ESP_BT_MODE_BLE;
#endif
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK){
        CY_LOGE(BLE_DB, "controller init failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK){
        CY_LOGE(BLE_DB, "controller enable failed: %s", esp_err_to_name(err));
        return -1;
    }
#if defined(CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE)
    /* Load-bearing, and easy to miss: nimble_port_init() reaches
     * esp_nimble_init() -> os_msys_init(), which initialises the mbuf pools but
     * does NOT allocate their backing buffers. esp_nimble_hci_init() is what
     * does that (ble_buf_alloc -> os_msys_buf_alloc). Skip it and
     * mem_init_mbuf_pool() gets a NULL buffer, trips
     * SYSINIT_PANIC_ASSERT(rc == 0) in os_msys_init.c and the device reboots in
     * a loop ~2.4 s after start. NimBLEDevice::init() calls it here too. */
    err = esp_nimble_hci_init();
    if(err != ESP_OK){
        CY_LOGE(BLE_DB, "nimble hci init failed: %s", esp_err_to_name(err));
        return -1;
    }
#endif

    if(nimble_port_init() != ESP_OK){
        CY_LOGE(BLE_DB, "nimble_port_init failed");
        return -1;
    }
#else
    esp_err_t err = nimble_port_init();     /* controller + host */
    if(err != ESP_OK){
        CY_LOGE(BLE_DB, "nimble_port_init failed: %s", esp_err_to_name(err));
        return -1;
    }
#endif
    return 0;
}

static void host_task(void *param){
    nimble_port_run();              /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_host_run(void){
    ble_store_config_init();
    nimble_port_freertos_init(host_task);   /* the mode's sync_cb fires from here */
}

int ble_host_resolve_addr(uint8_t *own_addr_type){
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0){
        CY_LOGE(BLE_DB, "no usable address, rc=%d", rc);
        return rc;
    }
    rc = ble_hs_id_infer_auto(0, own_addr_type);
    if(rc != 0){
        CY_LOGE(BLE_DB, "address type failed, rc=%d", rc);
    }
    return rc;
}

void ble_host_log_started(const char *mode, const char *name){
    CY_LOGI(BLE_DB, "BLE %s started as %s, free heap %" PRIu32, mode, name, esp_get_free_heap_size());
}

void ble_uuid128_from_base(ble_uuid128_t *out, uint16_t id){
    if(out == NULL){
        return;
    }
    out->u.type = BLE_UUID_TYPE_128;
    memcpy(out->value, cy_ble_base_uuid128, sizeof(out->value));
    /* Little-endian: the 16-bit slot sits at bytes 12/13, low byte first. */
    out->value[12] = (uint8_t)( id       & 0xFF);
    out->value[13] = (uint8_t)((id >> 8) & 0xFF);
}
