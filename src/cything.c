/*
 * CyThing library entry point. cything_begin() is everything the firmware
 * does at boot; the application (main/app_main.c for idf.py, a sketch for
 * Arduino/PlatformIO) calls it once and then only implements the command
 * hooks declared in CyThingEsp32.h. See doc/cy_thing_lib.md.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "CyThingEsp32.h"
#include "device_config/device_config.h"
#include "device_config/cy_config.h"
#include "common/task_config.h"
#include "common/reset_handler.h"
#include "memory_handler/flash_wifi_info_handler.h"
#include "ota_lib/flash_boot_handler.h"
#include "aws/provisioning.h"
#include "aws/mqtt_response_fifo.h"
#include "ble/ble_beacon.h"
#include "ble/ble_config.h"
#include "ble/ble_pairing.h"
#include "ble/discovery_mode.h"
#include "tcp_server/tcp_server.h"
#include "tcp_server/tcp_client_list.h"
#include "security/local_session.h"
#include "security/enc_frame.h"
#include "security/device_password.h"
#include "security/paired_list.h"
#include "security/pairing_events.h"
#include "tcp_server/tcp_response_fifo.h"
#include "tcp_server/pairing.h"
#include "udp_socket/udp_server.h"
#include "wifi/access_point.h"
#include "wifi/station_mode.h"
#include "wifi/wifi_info_handler.h"

/* The partition table is fixed at build time (partitions/cything-<size>.csv);
 * a 4 MB table runs fine on an 8/16 MB chip but leaves the rest unused. Say so
 * once at boot so the builder knows the larger variants exist. */
static void log_flash_vs_partition_table(void){
    uint32_t flash_size = 0;
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK){
        return;
    }
    uint32_t table_end = 0;
    /* esp_partition_next() releases the iterator itself when it returns NULL. */
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for( ; it != NULL; it = esp_partition_next(it)){
        const esp_partition_t *p = esp_partition_get(it);
        if(p->address + p->size > table_end){
            table_end = p->address + p->size;
        }
    }
    if(flash_size > table_end){
        ESP_LOGW(TAG, "flash is %lu MB but the partition table only uses %lu MB — see partitions/ for the larger variants",
                 (unsigned long)(flash_size >> 20), (unsigned long)((table_end + (1 << 20) - 1) >> 20));
    }
}

static esp_err_t init_nvs_flash(void){
  esp_err_t ret = nvs_flash_init();
  ESP_ERROR_CHECK(ret);
  return ret;

}

void cything_begin(void)
{

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %"PRIu32" bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    /* Which channel spec is actually in force — the one line that says whether
     * a dropped-in cything_config won over the library's weak defaults. */
    cy_config_log();
    log_flash_vs_partition_table();
    

    /* Response FIFOs must exist before any task can produce into them. */
    tcp_response_fifo_init();
    mqtt_response_fifo_init();
    tcp_client_list_init();
    local_session_init();
    enc_frame_init();
    pairing_timer_init();
    xTaskCreate(tcp_response_send_task, "tcp_response_send", TCP_RESPONSE_SEND_TASK_STACK_SIZE, NULL, TCP_RESPONSE_SEND_TASK_PRIORITY, NULL);

	if(init_nvs_flash() != ESP_OK){
		printf("#\r\nError NVS flash\r\n#\r\n");
	}

    provisioning_init();
    device_password_init();
    paired_list_init();
    discovery_mode_init();
    pairing_events_init();

    reset_handler();

	flash_read_boot_partition();

    flash_read_wifi_router_info();

    if(memcmp(flash_wifi_mode , WIFI_STA_MODE , 6) == 0){

        sprintf(ssid_arg , flash_router_ssid);//"GalaxyA35");//"Xiaomi326589");//flash_router_ssid);
        sprintf(pass_arg , flash_router_password);//"Majid4321");//flash_router_password);
        
#if BLE_SCAN_BEACON_ENABLED
        /* Station-mode scan beacon (doc/ble-scan-beacon.md): advertises the
         * LAN IP once the router hands one out — station_mode.c drives it
         * from the IP events. */
        ble_beacon_start();
#else
        /* Bluetooth is only used for pairing: reclaim its RAM. */
        ble_release_memory();
#endif

        xTaskCreate(wifi_init_sta, "wifi_init_sta_task", WIFI_STA_TASK_STACK_SIZE, NULL, WIFI_STA_TASK_PRIORITY, NULL);		
        wifi_if_mode = STA_MODE; 

    }else{

        wifi_init_accesspoint_mode();
        wifi_if_mode = AP_MODE;

        /* Pairable over BLE (doc/ble-pairing.md) and over the soft-AP's
         * TCP server (doc/pairing.md); both end in pairing_commit(). */
        ble_pairing_start();
        
        xTaskCreate(udp_server_task, "udp_server", UDP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, UDP_SERVER_TASK_PRIORITY, NULL);
        xTaskCreate(tcp_server_task, "tcp_server", TCP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, TCP_SERVER_TASK_PRIORITY, NULL);

    }

}
