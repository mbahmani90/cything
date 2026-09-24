#include "station_mode.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "device_config/device_config.h"
#include "aws/aws_config.h"
#include "common/cy_log.h"
#include "common/task_config.h"
#include "aws/aws_iot_handler.h"
#include "aws/provisioning.h"
#include "tcp_server/tcp_server.h"
#include "udp_socket/udp_server.h"
#include "wifi/wifi_info_handler.h"
#include "ble/ble_beacon.h"
#include "ble/ble_config.h"

EventGroupHandle_t s_wifi_event_group;

int  s_retry_num      = 0    ;
bool all_sockets_init = false;
char ssid_arg[128];
char pass_arg[128];

bool isConnectedToWifi = false;
int xre4 = 0;

int  wifi_if_mode= 1;

esp_netif_t *p_netif_sta;

void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        isConnectedToWifi = false;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        isConnectedToWifi = false;
#if BLE_SCAN_BEACON_ENABLED
        ble_beacon_on_ip_lost();            /* never advertise a stale address */
#endif
        // if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
             esp_wifi_connect();
        //     s_retry_num++;
             CY_LOGI(WIFI_STA_DB, "retry to connect to the AP");
        // } else {
        //     xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        // }
        CY_LOGI(WIFI_STA_DB, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        CY_LOGI(WIFI_STA_DB, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        // p_netif = *(&event->esp_netif);

#ifdef ENABLE_WIFI_STATIC_IP        
        if (esp_netif_dhcpc_stop(event->esp_netif) != ESP_OK) {

            isConnectedToWifi = true;
            if(!all_sockets_init){
                
                all_sockets_init = true;
                // Enable UDP Server
                xTaskCreate(udp_server_task, "udp_server", UDP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, UDP_SERVER_TASK_PRIORITY, NULL);
                // Enable TCP Server
                xTaskCreate(tcp_server_task, "tcp_server", TCP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, TCP_SERVER_TASK_PRIORITY, NULL);
                // Check Remote Server
                // if(isCheckingRemoteServerIdle){
                //     xTaskCreate(check_remote_server_status, "http_test", 4096, NULL, 5, NULL);
                // }
                // xTaskCreate(pingMainTask, "mainTask", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
            
            }     
            ESP_LOGE(TAG, "Failed to stop dhcp client");      
            return;
        }

        esp_netif_ip_info_t temp_ip_info = (event)->ip_info;

        const esp_ip4_addr_t *gw = &event->ip_info.gw;

        int unused_ip = find_unused_ip(esp_ip4_addr1(gw) , esp_ip4_addr2(gw) ,
                                        esp_ip4_addr3(gw) , esp_ip4_addr4(gw));

        if(unused_ip != -1){
            esp_netif_set_ip4_addr(&(temp_ip_info.ip) ,
                                    esp_ip4_addr1(gw) , esp_ip4_addr2(gw) ,
                                    esp_ip4_addr3(gw) , unused_ip);
            // esp_netif_set_ip4_addr(&(temp_ip_info.ip) , 172 , 16 , 225 , 13);      
            esp_err_t res = esp_netif_set_ip_info(event->esp_netif, &temp_ip_info);
            printf("STATIC IP RES: %X\r\n" , res);

            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&temp_ip_info.ip));
            wifi_update_device_ip(p_netif_sta);                
        }

#else
        wifi_update_device_ip(p_netif_sta);  
        isConnectedToWifi = true;
#if BLE_SCAN_BEACON_ENABLED
        ble_beacon_on_ip(&event->ip_info.ip);
#endif
        esp_wifi_set_ps(WIFI_PS_NONE);
        if(!all_sockets_init){
            all_sockets_init = true;            
            xTaskCreate(tcp_server_task, "tcp_server", TCP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, TCP_SERVER_TASK_PRIORITY, NULL);
            xTaskCreate(udp_server_task, "udp_server", UDP_SERVER_TASK_STACK_SIZE, (void*)AF_INET, UDP_SERVER_TASK_PRIORITY, NULL);          
        }

#endif        
        
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
    }
}

void wifi_init_sta(void *pvParameters)
{

	int ssid_arg_len = 0;
	int pass_arg_len = 0;

	for(int i = 0 ; i < 128 ; i++){
	
		if(ssid_arg[i] == '\0'){
			ssid_arg_len = i;
			break;
		}
		
	}

	for(int i = 0 ; i < 128 ; i++){
	
		if(pass_arg[i] == '\0'){
			pass_arg_len = i;
			break;
		}
		
	}

    printf("WIFI STA SSID: %s\r\n" , ssid_arg);
    printf("WIFI STA PASS: %s\r\n" , pass_arg);

    // //Initialize NVS
    // esp_err_t ret = nvs_flash_init();
    // if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    //   ESP_ERROR_CHECK(nvs_flash_erase());
    //   ret = nvs_flash_init();
    // }
    // ESP_ERROR_CHECK(ret);
    CY_LOGI(WIFI_STA_DB, "ESP_WIFI_MODE_STA");
    s_wifi_event_group = xEventGroupCreate();

     ESP_ERROR_CHECK(esp_netif_init());

     ESP_ERROR_CHECK(esp_event_loop_create_default());
     p_netif_sta = esp_netif_create_default_wifi_sta();

     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = {0},
            .password = {0},

               .pmf_cfg = {
                .capable = true,
                .required = false
            },         
        },
    };
	
	for(int i = 0; i < ssid_arg_len; i++){
		wifi_config.sta.ssid[i] = ssid_arg[i];
	}
	
	for(int i = 0; i < pass_arg_len; i++){
		wifi_config.sta.password[i] = pass_arg[i];
	}

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    CY_LOGI(WIFI_STA_DB, "wifi_init_sta finished.");
    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        //ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", ssid_arg, pass_arg);
        CY_LOGI(WIFI_STA_DB, "connected to AP");
        isConnectedToWifi = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
#if IS_REMOTE_CON_ENABLE == 1   
        vTaskDelay(3000 / portTICK_PERIOD_MS);

        // tcpip_adapter_dns_info_t dns_info;
        // tcpip_adapter_get_dns_info(TCPIP_ADAPTER_IF_STA, TCPIP_ADAPTER_DNS_MAIN, &dns_info);
        // printf("DNS: %s\n", ip4addr_ntoa(&dns_info.ip.u_addr.ip4));

        /* MQTT only runs on a provisioned device — it needs the per-device cert
         * from the CSR handshake. An unprovisioned unit stays off the broker. */
        if (provisioning_is_active()) {
           xTaskCreate(aws_iot_task, "aws_iot_task", AWS_IOT_TASK_STACK_SIZE, NULL, AWS_IOT_TASK_PRIORITY, NULL);
        } else {
            ESP_LOGI(TAG, "not provisioned — aws_iot_task not started");
        }
#endif
    } else if (bits & WIFI_FAIL_BIT) {
        //ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", ssid_arg, pass_arg);
        CY_LOGI(WIFI_STA_DB, "Failed to connect to AP");
        isConnectedToWifi = false;
    } else {
        CY_LOGE(WIFI_STA_DB, "UNEXPECTED EVENT");
        isConnectedToWifi = false;
    }

    /* The event will not be processed after unregister */
    //ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    //ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    //vEventGroupDelete(s_wifi_event_group);

    CY_LOGI(WIFI_STA_DB, "wifi_init_sta stack high-water mark: %u bytes free",
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

