/*
 * mDNS / DNS-SD discovery — see mdns_discovery.h and doc/mdns-discovery.md.
 *
 * The responder is the espressif/mdns component (a registry dependency under
 * ESP-IDF, precompiled into arduino-esp32 3.x). It runs its own task ("mdns",
 * CONFIG_MDNS_TASK_*: priority 1, CPU0, 4 KB) — below every CyThing task
 * except aws_iot_task — and, with CONFIG_MDNS_PREDEF_NETIF_STA, its own
 * WIFI_EVENT / IP_EVENT handlers for the default station netif. That is why
 * this file only starts it once and then just keeps the TXT record current.
 */
#include "mdns_discovery.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "mdns.h"

#include "device_config/cy_config.h"
#include "aws/provisioning.h"
#include "security/device_password.h"
#include "security/paired_list.h"
#include "wifi/wifi_info_handler.h"
#include "common/cy_log.h"

/* Library constants, not per channel: the app's Info.plist lists exactly this
 * type (NSBonjourServices), and iOS cannot browse anything else. */
#define MDNS_SERVICE_TYPE   "_cything"
#define MDNS_SERVICE_PROTO  "_tcp"
#define MDNS_HOSTNAME_PREFIX "cything-"
#define MDNS_INSTANCE_PREFIX "Cything "

/* The app generates ~8 chars [a-z0-9]; anything longer is cut here so the
 * instance name stays well under DNS's 63-byte label limit. */
#define MDNS_TAG_MAX        16

#define MDNS_TXT_ITEMS      7

static SemaphoreHandle_t s_lock;
static bool s_started = false;
static char s_tag[MDNS_TAG_MAX + 1];

/* cy_mdns_channel_tag, length-capped. Characters outside [a-z0-9] are kept
 * (TXT and instance names are UTF-8) but flagged, since the app compares the
 * tag byte for byte. */
static void load_channel_tag(void){
    snprintf(s_tag, sizeof(s_tag), "%s", cy_mdns_channel_tag);
    if(strlen(cy_mdns_channel_tag) > MDNS_TAG_MAX){
        CY_LOGW(MDNS_DB, "mdns: channel tag cut to %d chars", MDNS_TAG_MAX);
    }
    for(const char *p = s_tag ; *p ; p++){
        if(!(islower((unsigned char)*p) || isdigit((unsigned char)*p))){
            CY_LOGW(MDNS_DB, "mdns: channel tag '%s' is not [a-z0-9]", s_tag);
            break;
        }
    }
}

/* Everything in here is readable by anyone on the LAN: identity and state
 * flags only — never a password, key, token or account email. The value
 * buffers only need to live until the mdns call returns; it copies them. */
typedef struct {
    char device_id[PROV_DEVICE_ID_MAX];
    mdns_txt_item_t items[MDNS_TXT_ITEMS];
} txt_record_t;

static void build_txt(txt_record_t *txt){
    char source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
    char provision_state[16];
    provisioning_get_scan_fields(source_terminal_id, sizeof(source_terminal_id),
        txt->device_id, sizeof(txt->device_id), provision_state, sizeof(provision_state));

    txt->items[0] = (mdns_txt_item_t){ "id",     txt->device_id };
    txt->items[1] = (mdns_txt_item_t){ "ch",     s_tag };
    txt->items[2] = (mdns_txt_item_t){ "v",      cy_firmware_version };
    txt->items[3] = (mdns_txt_item_t){ "type",   cy_device_type };
    txt->items[4] = (mdns_txt_item_t){ "paired", paired_list_count() > 0   ? "1" : "0" };
    txt->items[5] = (mdns_txt_item_t){ "pw",     device_password_is_set()  ? "1" : "0" };
    txt->items[6] = (mdns_txt_item_t){ "prov",   provisioning_is_active()  ? "1" : "0" };
}

static void log_txt(const char *what, const txt_record_t *txt){
    CY_LOGI(MDNS_DB, "mdns: %s id=%s ch=%s v=%s type=%s paired=%s pw=%s prov=%s", what,
            txt->items[0].value, txt->items[1].value, txt->items[2].value, txt->items[3].value,
            txt->items[4].value, txt->items[5].value, txt->items[6].value);
    /* No stack high-water log for the "mdns" task: xTaskGetHandle() lives in
     * IRAM, and the IDF 5.5 / Arduino build has only a few hundred bytes of
     * IRAM left. */
}

void mdns_discovery_init(void){
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
}

void mdns_discovery_start(void){
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if(s_started){
        xSemaphoreGive(s_lock);
        return;
    }

#if !CONFIG_MDNS_PREDEF_NETIF_STA
    /* Without it the component would not follow the station's IP events and
     * would need mdns_register_netif() + mdns_netif_action() calls here. */
    CY_LOGW(MDNS_DB, "mdns: CONFIG_MDNS_PREDEF_NETIF_STA is off — not started");
    xSemaphoreGive(s_lock);
    return;
#endif

    load_channel_tag();

    char suffix[7];
    wifi_get_unit_suffix(suffix, sizeof(suffix));
    char hostname[sizeof(MDNS_HOSTNAME_PREFIX) + sizeof(suffix)];
    snprintf(hostname, sizeof(hostname), MDNS_HOSTNAME_PREFIX "%s", suffix);
    for(char *p = hostname ; *p ; p++){
        *p = (char)tolower((unsigned char)*p);
    }
    char instance[sizeof(MDNS_INSTANCE_PREFIX) + sizeof(suffix) + 2 + MDNS_TAG_MAX];
    if(s_tag[0] != 0){
        snprintf(instance, sizeof(instance), MDNS_INSTANCE_PREFIX "%s #%s", suffix, s_tag);
    }else{
        snprintf(instance, sizeof(instance), MDNS_INSTANCE_PREFIX "%s", suffix);
    }

    esp_err_t err = mdns_init();
    if(err != ESP_OK){
        CY_LOGE(MDNS_DB, "mdns: init failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return;
    }

    txt_record_t txt;
    build_txt(&txt);

    err = mdns_hostname_set(hostname);
    if(err == ESP_OK) err = mdns_instance_name_set(instance);
    if(err == ESP_OK) err = mdns_service_add(instance, MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO,
                                             cy_tcp_port, txt.items, MDNS_TXT_ITEMS);
    if(err != ESP_OK){
        CY_LOGE(MDNS_DB, "mdns: service setup failed: %s", esp_err_to_name(err));
        mdns_free();
        xSemaphoreGive(s_lock);
        return;
    }

    s_started = true;
    CY_LOGI(MDNS_DB, "mdns: advertising '%s' " MDNS_SERVICE_TYPE "." MDNS_SERVICE_PROTO " port %u on %s.local",
            instance, (unsigned)cy_tcp_port, hostname);
    log_txt("txt", &txt);
    xSemaphoreGive(s_lock);
}

void mdns_discovery_on_ip(void){
    if(s_started){
        CY_LOGI(MDNS_DB, "mdns: station up — responder re-announcing");
    }
}

void mdns_discovery_on_ip_lost(void){
    if(s_started){
        CY_LOGI(MDNS_DB, "mdns: station down — responder paused");
    }
}

void mdns_discovery_refresh(void){
    if(s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if(!s_started){
        xSemaphoreGive(s_lock);
        return;
    }
    txt_record_t txt;
    build_txt(&txt);
    esp_err_t err = mdns_service_txt_set(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, txt.items, MDNS_TXT_ITEMS);
    if(err != ESP_OK){
        CY_LOGW(MDNS_DB, "mdns: TXT update failed: %s", esp_err_to_name(err));
    }else{
        log_txt("txt updated", &txt);
    }
    xSemaphoreGive(s_lock);
}
