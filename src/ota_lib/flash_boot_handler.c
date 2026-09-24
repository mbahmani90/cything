#include "flash_boot_handler.h"

#include <string.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "common/partitions.h"
#include "common/cy_log.h"
#include "ota_lib/ota_handler.h"

esp_err_t flash_read_boot_partition(){
    
    ota_set_new_read_partition ((size_t) BOOT_PARTION_TYPE , (size_t) BOOT_PARTITION_SUBTYPE);

    char temp_boot_partition[FLASH_BOOT_PARTITION_SIZE + 1];

    esp_err_t result = ota_read(FLASH_BOOT_PARTITION_OFFSET , temp_boot_partition , FLASH_BOOT_PARTITION_SIZE);

    if(result != ESP_OK){
        return result;
    }

    CY_LOGI(FLASH_BOOT_DB, "Boot Partition: %.2X" , temp_boot_partition[1]);

    if(memcmp(temp_boot_partition , FLASH_BOOT_PARTITION_KEY , FLASH_BOOT_PARTITION_KEY_SIZE) == 0){
        
        /* Only the two OTA slots exist (partitions/cything-*.csv has no
         * factory partition), so anything else — including a stale FACTORY
         * request from an older table — is rejected and cleared. */
        if(temp_boot_partition[1] == ESP_PARTITION_SUBTYPE_APP_OTA_0   ||
           temp_boot_partition[1] == ESP_PARTITION_SUBTYPE_APP_OTA_1   ){

            if(ota_set_boot_firmware(temp_boot_partition[1]) == ESP_OK){
                flash_erase_boot_partition();
                esp_restart();
            }

        }else{
            CY_LOGW(FLASH_BOOT_DB, "boot partition requests unknown app slot %.2X; ignored", temp_boot_partition[1]);
            flash_erase_boot_partition();
        }

    }

    return ESP_OK;

}

esp_err_t flash_erase_boot_partition(){

    ota_set_new_erase_partition((size_t) BOOT_PARTION_TYPE , (size_t) BOOT_PARTITION_SUBTYPE);

    return ota_erase(FLASH_BOOT_PARTITION_OFFSET , 4096);

}

esp_err_t flash_erase_write_boot_partition(char boot_partition){

    ota_set_new_erase_write_partition((size_t) BOOT_PARTION_TYPE , (size_t) BOOT_PARTITION_SUBTYPE);

    char temp[8];
    temp[0] = FLASH_BOOT_PARTITION_KEY[0]; 
    temp[1] = boot_partition;
    return ota_erase_write(FLASH_BOOT_PARTITION_OFFSET , temp , 2);

}