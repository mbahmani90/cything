#include "wifi_trial.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "common/cy_log.h"
#include "wifi/wifi_info_handler.h"
#include "wifi/access_point.h"

#define TRIAL_GOT_IP_BIT   BIT0
#define TRIAL_FAILED_BIT   BIT1

static EventGroupHandle_t trial_events;
static esp_netif_t       *p_netif_trial;
static bool               trial_active;
static wifi_trial_result_t trial_failure;
static int                trial_retries;

/* Reasons that mean "no point retrying": the AP isn't there, or it refused
 * our key. Everything else (beacon timeout, assoc leave, …) gets a couple of
 * reconnects inside the timeout. */
static wifi_trial_result_t classify(uint8_t reason){
    switch(reason){
    case WIFI_REASON_NO_AP_FOUND:
#ifdef WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
#endif
        return WIFI_TRIAL_NOT_FOUND;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WIFI_TRIAL_AUTH_FAIL;
    default:
        return WIFI_TRIAL_TIMEOUT;   /* "keep trying" marker, see below */
    }
}

static void trial_event(void *arg, esp_event_base_t base, int32_t id, void *data){
    if(!trial_active){
        return;
    }
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED){
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        wifi_trial_result_t why = classify(e->reason);
        CY_LOGI(WIFI_STA_DB, "trial: disconnected, reason %d", e->reason);
        if(why == WIFI_TRIAL_TIMEOUT && trial_retries-- > 0){
            esp_wifi_connect();
            return;
        }
        trial_failure = why;
        xEventGroupSetBits(trial_events, TRIAL_FAILED_BIT);
    }else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        CY_LOGI(WIFI_STA_DB, "trial: got ip " IPSTR, IP2STR(&e->ip_info.ip));
        wifi_update_device_ip(p_netif_trial);
        xEventGroupSetBits(trial_events, TRIAL_GOT_IP_BIT);
    }
}

static esp_err_t trial_init_once(void){
    if(trial_events != NULL){
        return ESP_OK;
    }
    trial_events = xEventGroupCreate();
    if(trial_events == NULL){
        return ESP_ERR_NO_MEM;
    }
    /* esp_netif_init() / the default event loop are already up from the
     * soft-AP bring-up (access_point.c). */
    p_netif_trial = esp_netif_create_default_wifi_sta();
    if(p_netif_trial == NULL){
        return ESP_FAIL;
    }
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                        &trial_event, NULL, NULL);
    if(err == ESP_OK){
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &trial_event, NULL, NULL);
    }
    return err;
}

wifi_trial_result_t wifi_trial_run(const char *ssid, const char *password, unsigned timeout_ms){
    if(trial_init_once() != ESP_OK){
        CY_LOGE(WIFI_STA_DB, "trial: init failed");
        return WIFI_TRIAL_ERROR;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    if(password[0] != 0){
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    xEventGroupClearBits(trial_events, TRIAL_GOT_IP_BIT | TRIAL_FAILED_BIT);
    trial_failure = WIFI_TRIAL_TIMEOUT;
    trial_retries = 2;
    trial_active  = true;

    /* Keep the soft-AP up (a phone on the TCP path stays connected) and add
     * the station beside it. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if(err == ESP_OK){
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if(err == ESP_OK){
        err = esp_wifi_connect();
    }
    if(err != ESP_OK){
        CY_LOGE(WIFI_STA_DB, "trial: start failed: %s", esp_err_to_name(err));
        trial_active = false;
        esp_wifi_set_mode(WIFI_MODE_AP);
        return WIFI_TRIAL_ERROR;
    }
    CY_LOGI(WIFI_STA_DB, "trial: connecting to \"%s\"", ssid);

    EventBits_t bits = xEventGroupWaitBits(trial_events, TRIAL_GOT_IP_BIT | TRIAL_FAILED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    trial_active = false;

    if(bits & TRIAL_GOT_IP_BIT){
        return WIFI_TRIAL_OK;
    }
    wifi_trial_result_t result = (bits & TRIAL_FAILED_BIT) ? trial_failure : WIFI_TRIAL_TIMEOUT;
    CY_LOGW(WIFI_STA_DB, "trial: failed (%d)", result);
    /* Back to plain AP mode; the soft-AP's own address is the device IP again. */
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_update_device_ip(p_netif_ap);
    return result;
}
