#include "flash_wifi_info_handler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common/partitions.h"
#include "ota_lib/ota_handler.h"

char flash_wifi_mode      [FLASH_WIFI_MODE_SIZE      ] = {0xFF};
char flash_router_ssid    [FLASH_ROUTER_SSID_SIZE    ] = {0xFF};
char flash_router_password[FLASH_ROUTER_PASSWORD_SIZE] = {0xFF};

esp_err_t flash_store_wifi_router_info(char *ssid , char *password , char *new_wifi_mode , int ssid_len, int password_len){

  ota_set_new_erase_partition((size_t) WIFI_INFO_PARTION_TYPE , (size_t) WIFI_INFO_PARTITION_SUBTYPE);
  ota_set_new_write_partition((size_t) WIFI_INFO_PARTION_TYPE , (size_t) WIFI_INFO_PARTITION_SUBTYPE);
  
  esp_err_t result = flash_read_wifi_router_info();
  if(result != ESP_OK){
    return result;
  }

  result = ota_erase(FLASH_WIFI_MODE_OFFSET , 4096);
  if(result != ESP_OK){
    return result;
  }

  vTaskDelay(1 / portTICK_PERIOD_MS);

  result = ota_write(FLASH_WIFI_MODE_OFFSET , new_wifi_mode , FLASH_WIFI_MODE_SIZE);
  if(result != ESP_OK){
    return result;
  }  

  result = ota_write(FLASH_ROUTER_SSID_OFFSET , ssid , ssid_len);
  if(result != ESP_OK){
    return result;
  }  

  result = ota_write(FLASH_ROUTER_PASSWORD_OFFSET , password , password_len);
  if(result != ESP_OK){
    return result;
  }  

  return result;

}

esp_err_t flash_read_wifi_router_info(){

  ota_set_new_read_partition((size_t) WIFI_INFO_PARTION_TYPE , (size_t) WIFI_INFO_PARTITION_SUBTYPE);

  vTaskDelay(1 / portTICK_PERIOD_MS);
  
  esp_err_t result = ota_read(FLASH_WIFI_MODE_OFFSET , (char *) flash_wifi_mode , FLASH_WIFI_MODE_SIZE);
  if(result != ESP_OK){
    return result;
  }

  result = ota_read(FLASH_ROUTER_SSID_OFFSET , flash_router_ssid , FLASH_ROUTER_SSID_SIZE);
  if(result != ESP_OK){
    return result;
  }

  result = ota_read(FLASH_ROUTER_PASSWORD_OFFSET , flash_router_password , FLASH_ROUTER_PASSWORD_SIZE);
  if(result != ESP_OK){
    return result;
  }

  return result;

}