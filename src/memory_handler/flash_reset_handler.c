#include "flash_reset_handler.h"

#include <string.h>
#include "common/partitions.h"
#include "ota_lib/ota_handler.h"

int registeration_reset_counter = 0;

esp_err_t flash_read_reset_counter(){
    
    ota_set_new_read_partition((size_t) RESET_COUNTER_PARTITION_TYPE , (size_t) RESET_COUNTER_PARTITION_SUBTYPE);
    
    char temp_counter[RESET_COUNTER_SIZE + 1];

    esp_err_t result = ota_read(RESET_COUNTER_OFFSET , temp_counter , RESET_COUNTER_SIZE);

    if(result != ESP_OK){
        return result;
    }

    if(memcmp(temp_counter , RESET_COUNTER_KEY , RESET_COUNTER_KEY_SIZE) == 0){
        if(temp_counter[1] != FLASH_EMPTY_VALUE){
            registeration_reset_counter = temp_counter[1];
        }else{
            registeration_reset_counter = 0;
        }
    }else{
        registeration_reset_counter = 0;
    }

    return result;

}

esp_err_t flash_erase_write_reset_counter(int new_reset_counter){
    
    ota_set_new_erase_write_partition((size_t) RESET_COUNTER_PARTITION_TYPE , (size_t) RESET_COUNTER_PARTITION_SUBTYPE);
    
    char temp_counter[RESET_COUNTER_SIZE + 1] = {0};

    int i = 0;
    for(i = 0 ; i < RESET_COUNTER_KEY_SIZE ; i++){
        temp_counter[i] = RESET_COUNTER_KEY[i];
    }

    temp_counter[i] = new_reset_counter & 0xFF;

    esp_err_t result = ota_erase_write(RESET_COUNTER_OFFSET , temp_counter , RESET_COUNTER_SIZE);

    if(result != ESP_OK){
        return result;
    }

    registeration_reset_counter = new_reset_counter;

    return result;

}