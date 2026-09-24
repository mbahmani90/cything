/*
 * BLE Wi-Fi pairing — NimBLE GATT server, see doc/ble-pairing.md. Controller
 * and host bring-up are shared with the station-mode beacon (ble_host.c);
 * this file owns the pairing mode only.
 *
 * Two services:
 *   - the custom pairing service (ble_config.h UUIDs): SSID, Password and
 *     Control are write-only; Status and Info are read + notify.
 *   - SIG Device Information (0x180A): model, firmware and hardware
 *     revision, read-only.
 *
 * NimBLE hands a write to the access callback only once it is complete
 * (short writes and prepare/execute long writes alike) and tracks the
 * CCCD subscriptions itself, so this file is just the attribute table,
 * the access callback and the advertising/connection bookkeeping.
 *
 * All callbacks run on the NimBLE host task. COMMIT is handled on a
 * short-lived task of our own (BLE_COMMIT_TASK_*): it blocks for the Wi-Fi
 * trial and the flash write, neither of which may stall the host.
 */
#include "ble_pairing.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "ble/ble_nimble.h"
#include "ble/ble_host.h"
#include "ble/ble_config.h"
#include "common/cy_log.h"
#include "common/task_config.h"
#include "device_config/device_config.h"
#include "tcp_server/pairing.h"
#include "udp_socket/udp_response_handler.h"
#include "udp_socket/udp_server.h"
#include "wifi/wifi_info_handler.h"
#include "wifi/wifi_trial.h"
#include "device_config/cy_config.h"

#define DIS_SERVICE_UUID    0x180A
#define DIS_MODEL_UUID      0x2A24
#define DIS_FW_REV_UUID     0x2A26
#define DIS_HW_REV_UUID     0x2A27

/* --------------------------------------------------------------- state --- */

static uint8_t  own_addr_type;
static uint16_t conn_handle_saved = BLE_HS_CONN_HANDLE_NONE;
static bool     commit_in_progress = false;

static uint8_t  status_value[2] = { BLE_PAIR_STATE_IDLE, 0 };
static uint16_t status_val_handle;
static uint16_t info_val_handle;

/* Credentials as received; +1 so they are NUL-terminated for pairing_commit. */
static char cred_ssid[BLE_PAIR_SSID_MAX + 1];
static char cred_pass[BLE_PAIR_PASS_MAX + 1];
static bool have_ssid = false;

/* ------------------------------------------------------ attribute table --- */

/* Which characteristic an access callback is for (the `arg` of each
 * ble_gatt_chr_def). */
enum {
    CHR_SSID, CHR_PASS, CHR_CTRL, CHR_STATUS, CHR_INFO,
    CHR_DIS_MODEL, CHR_DIS_FW, CHR_DIS_HW,
};

/*
 * Not const and not initialized here: the base UUID is the channel's, read at
 * runtime from cy_ble_base_uuid128, so init_uuids() fills these before the
 * GATT table is registered or anything is advertised. The gatt_svcs table
 * below still takes their addresses, which are as constant as ever.
 */
static ble_uuid128_t pair_svc_uuid;
static ble_uuid128_t pair_ssid_uuid;
static ble_uuid128_t pair_pass_uuid;
static ble_uuid128_t pair_ctrl_uuid;
static ble_uuid128_t pair_status_uuid;
static ble_uuid128_t pair_info_uuid;

static void init_uuids(void){
    ble_uuid128_from_base(&pair_svc_uuid,    BLE_PAIRING_SVC_ID);
    ble_uuid128_from_base(&pair_ssid_uuid,   BLE_PAIRING_SSID_ID);
    ble_uuid128_from_base(&pair_pass_uuid,   BLE_PAIRING_PASS_ID);
    ble_uuid128_from_base(&pair_ctrl_uuid,   BLE_PAIRING_CTRL_ID);
    ble_uuid128_from_base(&pair_status_uuid, BLE_PAIRING_STATUS_ID);
    ble_uuid128_from_base(&pair_info_uuid,   BLE_PAIRING_INFO_ID);
}

#if BLE_PAIRING_REQUIRE_ENCRYPTION
#define PAIR_WRITE_FLAGS (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#else
#define PAIR_WRITE_FLAGS (BLE_GATT_CHR_F_WRITE)
#endif

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &pair_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &pair_ssid_uuid.u,   .access_cb = gatt_access, .arg = (void *)CHR_SSID,
              .flags = PAIR_WRITE_FLAGS },
            { .uuid = &pair_pass_uuid.u,   .access_cb = gatt_access, .arg = (void *)CHR_PASS,
              .flags = PAIR_WRITE_FLAGS },
            { .uuid = &pair_ctrl_uuid.u,   .access_cb = gatt_access, .arg = (void *)CHR_CTRL,
              .flags = PAIR_WRITE_FLAGS },
            { .uuid = &pair_status_uuid.u, .access_cb = gatt_access, .arg = (void *)CHR_STATUS,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &status_val_handle },
            /* The same CSV the UDP GET_INFO scan returns, so the app can run
             * its post-pairing steps by IP without a multicast scan. The
             * notify is only a hint (it truncates at MTU-3); read for the
             * full value. */
            { .uuid = &pair_info_uuid.u,   .access_cb = gatt_access, .arg = (void *)CHR_INFO,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &info_val_handle },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(DIS_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(DIS_MODEL_UUID),  .access_cb = gatt_access, .arg = (void *)CHR_DIS_MODEL,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(DIS_FW_REV_UUID), .access_cb = gatt_access, .arg = (void *)CHR_DIS_FW,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(DIS_HW_REV_UUID), .access_cb = gatt_access, .arg = (void *)CHR_DIS_HW,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

/* -------------------------------------------------------------- status --- */

static void set_status(uint8_t state, uint8_t detail){
    status_value[0] = state;
    status_value[1] = detail;
    /* Notifies every subscribed peer (NimBLE reads the value back through
     * gatt_access); a no-op when nobody subscribed. */
    ble_gatts_chr_updated(status_val_handle);
    CY_LOGI(BLE_DB, "status -> 0x%02X/0x%02X", state, detail);
}

/* -------------------------------------------------------------- commit --- */

/* Try the credentials first (APSTA), then store + reboot only on success —
 * a typo is reported on Status instead of bricking the device into a station
 * mode that never connects. */
static void ble_commit_task(void *arg){
    set_status(BLE_PAIR_STATE_TRYING, 0);
    wifi_trial_result_t trial = wifi_trial_run(cred_ssid, cred_pass, BLE_PAIRING_WIFI_TRIAL_TIMEOUT_MS);
    if(trial != WIFI_TRIAL_OK){
        uint8_t detail = trial == WIFI_TRIAL_NOT_FOUND ? BLE_PAIR_ERR_WIFI_NOT_FOUND
                       : trial == WIFI_TRIAL_AUTH_FAIL ? BLE_PAIR_ERR_WIFI_AUTH
                       :                                  BLE_PAIR_ERR_WIFI_TIMEOUT;
        set_status(BLE_PAIR_STATE_ERROR, detail);
        commit_in_progress = false;   /* still pairable; the app may retry */
        vTaskDelete(NULL);
        return;
    }

    /* Joined: Info now carries the router-assigned IP. Give the phone a
     * moment to read it before the flash write and the reboot that follows. */
    ble_gatts_chr_updated(info_val_handle);
    set_status(BLE_PAIR_STATE_WIFI_JOINED, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_err_t err = pairing_commit(cred_ssid, cred_pass);
    if(err == ESP_OK){
        /* pairing_commit() has armed the 1 s reboot; the notify leaves first. */
        set_status(BLE_PAIR_STATE_STORED, 0);
    }else{
        set_status(BLE_PAIR_STATE_ERROR, BLE_PAIR_ERR_FLASH);
        commit_in_progress = false;
    }
    vTaskDelete(NULL);
}

static void handle_control(uint8_t opcode){
    switch(opcode){
    case BLE_PAIR_CTRL_COMMIT:
        if(commit_in_progress){
            set_status(BLE_PAIR_STATE_ERROR, BLE_PAIR_ERR_BUSY);
        }else if(!have_ssid){
            set_status(BLE_PAIR_STATE_ERROR, BLE_PAIR_ERR_NO_SSID);
        }else{
            commit_in_progress = true;
            CY_LOGI(BLE_DB, "commit: ssid=\"%s\" password=%u chars", cred_ssid, (unsigned)strlen(cred_pass));
            if(xTaskCreate(ble_commit_task, "ble_commit", BLE_COMMIT_TASK_STACK_SIZE, NULL,
                           BLE_COMMIT_TASK_PRIORITY, NULL) != pdPASS){
                commit_in_progress = false;
                set_status(BLE_PAIR_STATE_ERROR, BLE_PAIR_ERR_FLASH);
            }
        }
        break;
    default:
        set_status(BLE_PAIR_STATE_ERROR, BLE_PAIR_ERR_BAD_OPCODE);
        break;
    }
}

/* ------------------------------------------------------ access callback --- */

/* Copy a complete written value out of the mbuf chain into `dst`
 * (NUL-terminated). Returns an ATT error code, 0 on success. */
static int take_write(struct os_mbuf *om, char *dst, uint16_t max_len, uint16_t min_len, uint16_t *out_len){
    uint16_t len = OS_MBUF_PKTLEN(om);
    if(len < min_len || len > max_len){
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if(ble_hs_mbuf_to_flat(om, dst, max_len, &len) != 0){
        return BLE_ATT_ERR_UNLIKELY;
    }
    dst[len] = 0;
    *out_len = len;
    return 0;
}

static int append_read(struct os_mbuf *om, const void *data, size_t len){
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg){
    int which = (int)(intptr_t)arg;
    uint16_t len;
    int rc;

    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR){
        switch(which){
        case CHR_SSID:
            rc = take_write(ctxt->om, cred_ssid, BLE_PAIR_SSID_MAX, 1, &len);
            if(rc == 0){
                have_ssid = true;
                CY_LOGI(BLE_DB, "ssid <- \"%s\"", cred_ssid);
                if(!commit_in_progress){
                    set_status(BLE_PAIR_STATE_READY, 0);
                }
            }
            return rc;
        case CHR_PASS:
            rc = take_write(ctxt->om, cred_pass, BLE_PAIR_PASS_MAX, 0, &len);
            if(rc == 0){
                CY_LOGI(BLE_DB, "password <- %u chars", (unsigned)len);
            }
            return rc;
        case CHR_CTRL: {
            char opcode[2];
            rc = take_write(ctxt->om, opcode, 1, 1, &len);
            if(rc == 0){
                handle_control((uint8_t)opcode[0]);
            }
            return rc;
        }
        default:
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
    }

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR){
        switch(which){
        case CHR_STATUS:    return append_read(ctxt->om, status_value, sizeof(status_value));
        case CHR_INFO: {
            static char info[UDP_REPLY_MAX];   /* host task only; keep it off its 4 KB stack */
            int n = udp_get_info_response(info, sizeof(info));
            return append_read(ctxt->om, info, n > 0 ? (size_t)n : 0);
        }
        case CHR_DIS_MODEL: return append_read(ctxt->om, cy_device_type, strlen(cy_device_type));
        case CHR_DIS_FW:    return append_read(ctxt->om, cy_firmware_version, strlen(cy_firmware_version));
        case CHR_DIS_HW:    return append_read(ctxt->om, cy_hardware_version, strlen(cy_hardware_version));
        default:            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* --------------------------------------------------------- advertising --- */

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void){
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    /* Advertising packet: flags + the 128-bit service UUID (iOS only filters
     * on the advertising packet, never on the scan response). */
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&pair_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if(rc != 0){
        CY_LOGE(BLE_DB, "adv fields failed, rc=%d", rc);
        return;
    }

    /* Scan response: the name (both don't fit in 31 bytes). */
    const char *name = ble_svc_gap_device_name();
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if(rc != 0){
        CY_LOGE(BLE_DB, "scan response fields failed, rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(BLE_PAIRING_ADV_INTERVAL_MIN_MS);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(BLE_PAIRING_ADV_INTERVAL_MAX_MS);
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if(rc != 0){
        CY_LOGE(BLE_DB, "advertising start failed, rc=%d", rc);
        return;
    }
    CY_LOGI(BLE_DB, "advertising as %s", name);
}

static int gap_event(struct ble_gap_event *event, void *arg){
    switch(event->type){
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0){
            conn_handle_saved = event->connect.conn_handle;
            CY_LOGI(BLE_DB, "connected, handle %d", event->connect.conn_handle);
        }else{
            CY_LOGW(BLE_DB, "connection failed, status %d", event->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle_saved = BLE_HS_CONN_HANDLE_NONE;
        CY_LOGI(BLE_DB, "disconnected, reason %d", event->disconnect.reason);
        /* Advertising stops on connect; resume so the next attempt finds us
         * (unless we are about to reboot anyway). */
        if(!commit_in_progress){
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if(!commit_in_progress){
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.status == 0){
            CY_LOGI(BLE_DB, "link encrypted");
        }else{
            CY_LOGW(BLE_DB, "pairing failed, status %d", event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone still holds a bond from an earlier life of this device:
         * drop our side and let it pair again. */
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0){
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle == status_val_handle){
            CY_LOGI(BLE_DB, "status notifications %s", event->subscribe.cur_notify ? "on" : "off");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        CY_LOGI(BLE_DB, "MTU %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------- host --- */

static void on_reset(int reason){
    CY_LOGE(BLE_DB, "host reset, reason %d", reason);
}

static void on_sync(void){
    if(ble_host_resolve_addr(&own_addr_type) != 0){
        return;
    }
    start_advertising();
}

void ble_pairing_start(void){
    int rc;

    if(ble_host_init() != 0){
        return;
    }

    /* Before ble_gatts_count_cfg() walks gatt_svcs and before start_advertising()
     * puts pair_svc_uuid in the advertising packet. */
    init_uuids();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* LE Secure Connections, no I/O -> Just Works; no bonding so a re-flashed
     * device never fights a stale bond on the phone. Only matters when a
     * characteristic asks for encryption (BLE_PAIRING_REQUIRE_ENCRYPTION). */
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(gatt_svcs);
    if(rc == 0){
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if(rc != 0){
        CY_LOGE(BLE_DB, "GATT table failed, rc=%d", rc);
        return;
    }

    char name[sizeof(BLE_PAIRING_NAME_PREFIX) + 6];
    char suffix[7];
    wifi_get_unit_suffix(suffix, sizeof(suffix));
    snprintf(name, sizeof(name), "%s%s", BLE_PAIRING_NAME_PREFIX, suffix);
    ble_svc_gap_device_name_set(name);

    ble_host_run();                         /* advertising starts from on_sync */
    ble_host_log_started("pairing", name);
}

void ble_release_memory(void){
    uint32_t before = esp_get_free_heap_size();
    esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    CY_LOGI(BLE_DB, "BT memory released (%s): free heap %" PRIu32 " -> %" PRIu32,
            esp_err_to_name(err), before, esp_get_free_heap_size());
}

#ifdef ARDUINO
/* arduino-esp32 frees the BLE controller RAM before setup() unless something
 * claims it; this strong definition overrides the core's weak default so
 * ble_pairing_start() still finds the memory in place. */
bool bleInUse(void){
    return true;
}
#endif
