/*
 * Station-mode scan beacon — NimBLE broadcaster, no GATT. See ble_beacon.h
 * and doc/ble-scan-beacon.md. Shares the controller / host bring-up with
 * the pairing service through ble_host.c; the two modes never coexist.
 *
 * Two flags gate advertising: the host must have synced (sync_cb, host
 * task) and the station must hold an IP (ble_beacon_on_ip, esp event loop
 * task). Whichever arrives second starts the advertiser; a mutex keeps the
 * stop/start pair atomic across the two tasks.
 */
#include "ble_beacon.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ble/ble_nimble.h"
#include "ble/ble_config.h"
#include "ble/ble_host.h"
#include "ble/discovery_mode.h"
#include "aws/provisioning.h"
#include "common/cy_log.h"
#include "wifi/wifi_info_handler.h"

/* One AD structure holds at most 29 bytes of data (31 - length - type). */
#define BEACON_NAME_MAX 29

/* Manufacturer Specific Data: company id LE, payload version, IPv4. */
#define MFG_LEN (2 + 1 + 4)

/* Filled in ble_beacon_start(): the base UUID is the channel's now. */
static ble_uuid128_t scan_svc_uuid;

static SemaphoreHandle_t lock;
static uint8_t  own_addr_type;
static bool     host_ready = false;
static bool     have_ip = false;
static bool     advertising = false;
static uint8_t  mfg_data[MFG_LEN] = {
    (uint8_t)(BLE_BEACON_COMPANY_ID & 0xFF), (uint8_t)(BLE_BEACON_COMPANY_ID >> 8),
    BLE_BEACON_PAYLOAD_VER, 0, 0, 0, 0
};
static char     adv_name[BEACON_NAME_MAX + 1];
static bool     adv_name_complete = true;

/* Identity for the scan response: the provisioned deviceId, else the unit
 * suffix the soft-AP / pairing name also carry. Fixed for this boot — a
 * provisioning handshake ends in a reboot (aws/provisioning.c PFIN). */
static void build_name(void){
    char source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
    char device_id[PROV_DEVICE_ID_MAX];
    char provision_state[16];
    provisioning_get_scan_fields(source_terminal_id, sizeof(source_terminal_id),
                                 device_id, sizeof(device_id),
                                 provision_state, sizeof(provision_state));
    if(device_id[0] != '\0'){
        adv_name_complete = strlen(device_id) <= BEACON_NAME_MAX;
        snprintf(adv_name, sizeof(adv_name), "%.*s", BEACON_NAME_MAX, device_id);
    }else{
        char suffix[7];
        wifi_get_unit_suffix(suffix, sizeof(suffix));
        snprintf(adv_name, sizeof(adv_name), "%s%s", BLE_PAIRING_NAME_PREFIX, suffix);
        adv_name_complete = true;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg);

/* Caller holds `lock`. */
static void stop_advertising_locked(void){
    if(!advertising){
        return;
    }
    int rc = ble_gap_adv_stop();
    if(rc != 0 && rc != BLE_HS_EALREADY){
        CY_LOGW(BLE_DB, "beacon stop failed, rc=%d", rc);
    }
    advertising = false;
}

/* Caller holds `lock`. Restarts if already running so a new IP takes
 * effect — advertising data cannot be changed while the set is active. */
static void start_advertising_locked(void){
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    if(!host_ready || !have_ip){
        return;
    }

    if(!discovery_mode_should_advertise_ble()){
        CY_LOGI(BLE_DB, "beacon off: discovery mode is Wi-Fi only");
        stop_advertising_locked();
        return;
    }

    stop_advertising_locked();

    /* Advertising packet: 3 (flags) + 18 (UUID) + 9 (mfg) = 30 of 31 bytes.
     * The UUID must sit here, not in the scan response: iOS and Android's
     * hardware filters only look at the advertising packet. */
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&scan_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);
    rc = ble_gap_adv_set_fields(&fields);
    if(rc != 0){
        CY_LOGE(BLE_DB, "beacon adv fields failed, rc=%d", rc);
        return;
    }

    /* Scan response: the identity as the local name (up to 29 bytes). */
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)adv_name;
    rsp_fields.name_len = strlen(adv_name);
    rsp_fields.name_is_complete = adv_name_complete;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if(rc != 0){
        CY_LOGE(BLE_DB, "beacon scan response failed, rc=%d", rc);
        return;
    }

    /* Non-connectable + scannable (ADV_SCAN_IND): nothing to connect to,
     * but an active scanner still gets the name. */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(BLE_BEACON_ADV_INTERVAL_MIN_MS);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(BLE_BEACON_ADV_INTERVAL_MAX_MS);
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if(rc != 0){
        CY_LOGE(BLE_DB, "beacon start failed, rc=%d", rc);
        return;
    }
    advertising = true;
    CY_LOGI(BLE_DB, "beacon advertising as %s at %u.%u.%u.%u", adv_name,
            mfg_data[3], mfg_data[4], mfg_data[5], mfg_data[6]);
}

static int gap_event(struct ble_gap_event *event, void *arg){
    switch(event->type){
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Not expected with BLE_HS_FOREVER; resume if the controller ever
         * ends the set on its own. */
        xSemaphoreTake(lock, portMAX_DELAY);
        advertising = false;
        start_advertising_locked();
        xSemaphoreGive(lock);
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------- host --- */

static void on_reset(int reason){
    CY_LOGE(BLE_DB, "host reset, reason %d", reason);
    xSemaphoreTake(lock, portMAX_DELAY);
    host_ready = false;
    advertising = false;
    xSemaphoreGive(lock);
}

static void on_sync(void){
    if(ble_host_resolve_addr(&own_addr_type) != 0){
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    host_ready = true;
    start_advertising_locked();
    xSemaphoreGive(lock);
}

void ble_beacon_start(void){
    if(lock == NULL){
        lock = xSemaphoreCreateMutex();
    }
    ble_uuid128_from_base(&scan_svc_uuid, BLE_SCAN_SVC_ID);
    if(ble_host_init() != 0){
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* The host wants the GAP/GATT services registered even though nobody
     * can connect; the GAP name is what a generic scanner app shows. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    build_name();
    ble_svc_gap_device_name_set(adv_name);

    ble_host_run();                         /* advertising starts from on_sync */
    ble_host_log_started("beacon", adv_name);
}

void ble_beacon_on_ip(const esp_ip4_addr_t *ip){
    if(lock == NULL){
        return;                             /* ble_beacon_start() not called (or failed early) */
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    mfg_data[3] = esp_ip4_addr1(ip);
    mfg_data[4] = esp_ip4_addr2(ip);
    mfg_data[5] = esp_ip4_addr3(ip);
    mfg_data[6] = esp_ip4_addr4(ip);
    have_ip = true;
    start_advertising_locked();
    xSemaphoreGive(lock);
}

void ble_beacon_on_ip_lost(void){
    if(lock == NULL){
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    have_ip = false;
    stop_advertising_locked();
    xSemaphoreGive(lock);
}

void ble_beacon_apply_discovery_mode(void){
    if(lock == NULL){
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    start_advertising_locked();             /* stops itself when the mode excludes BLE */
    xSemaphoreGive(lock);
}
