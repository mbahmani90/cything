#include "ota_handler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "common/cy_log.h"

esp_partition_t *new_write_partition;
esp_partition_t *new_erase_partition;
esp_partition_t *new_read_partition ;
esp_partition_t *new_erase_write_partition;

int last_erase_offset = 0;

void ota_read_otadata_partition(){

  ota_set_new_read_partition((size_t) ESP_PARTITION_TYPE_DATA , (size_t) ESP_PARTITION_SUBTYPE_DATA_OTA);
  char temp_otadata[128] = {0};
  ota_read(0 , temp_otadata , 127);

  CY_LOGI(OTA_DB, "OTA data: %.2X %.2X %.2X %.2X %.2X %.2X %.2X " , temp_otadata[0] , 
     temp_otadata[1] , temp_otadata[2] , temp_otadata[3], temp_otadata[4] , temp_otadata[5] , temp_otadata[6]);

}

void ota_read_ota_1_partition(){

  ota_set_new_read_partition((size_t) ESP_PARTITION_TYPE_APP , (size_t) ESP_PARTITION_SUBTYPE_APP_OTA_1);
  char temp_ota_1_data[2048] = {0};

  for(int j = 0 ; j < 10 ; j++){

    ota_read( (j * 1024) , temp_ota_1_data , 1024);
    for(int i = 0 ; i < 1024 ; i ++){
      printf("%.2X " , temp_ota_1_data[i]);    
      if( !(i & 0xF) && i ){
        printf("\r\n");
      } 
    }
    printf("\r\n");  
    vTaskDelay(20 / portTICK_PERIOD_MS);

  }

}

esp_err_t ota_set_boot_firmware(int partition){

  ota_set_new_erase_write_partition((size_t) ESP_PARTITION_TYPE_APP , (size_t) partition);
  return esp_ota_set_boot_partition(new_erase_write_partition);
  CY_LOGI(OTA_DB, "Set boot firmware");
}

void ota_set_new_write_partition(size_t type , size_t subtype){
  esp_partition_iterator_t temp_esp_partition_iterator = esp_partition_find((esp_partition_type_t) type , (esp_partition_subtype_t) subtype , NULL);
  new_write_partition = (esp_partition_t *) esp_partition_get(temp_esp_partition_iterator);
}

void ota_set_new_erase_partition(size_t type , size_t subtype){
  new_erase_partition = (esp_partition_t *) esp_partition_get(esp_partition_find((esp_partition_type_t) type , (esp_partition_subtype_t) subtype , NULL));
}

void ota_set_new_read_partition(size_t type , size_t subtype){
  esp_partition_iterator_t temp_esp_partition_iterator = esp_partition_find((esp_partition_type_t) type , (esp_partition_subtype_t) subtype , NULL);
  new_read_partition = (esp_partition_t *) esp_partition_get(temp_esp_partition_iterator);
}

void ota_set_new_erase_write_partition(size_t type , size_t subtype){
  new_erase_write_partition = (esp_partition_t *) esp_partition_get(esp_partition_find((esp_partition_type_t) type , (esp_partition_subtype_t) subtype , NULL));
}

esp_err_t ota_write(long int offset , char *data , long int data_length){
  return esp_partition_write((const esp_partition_t *) new_write_partition, offset , data , data_length);
}

esp_err_t ota_erase(size_t offset , size_t length){
  return esp_partition_erase_range((const esp_partition_t *)new_erase_partition, offset, length);
}

esp_err_t ota_read(long int offset , char *data , long int data_length){
  return esp_partition_read((const esp_partition_t *) new_read_partition, offset , data , data_length);
}

esp_err_t ota_erase_write(long int offset , char *data , long int data_length){
  esp_err_t err;
  CY_LOGI(OTA_DB, "ota_erase_write: %ld" , new_erase_write_partition->erase_size);

  if( (offset + data_length) >= last_erase_offset){

      do{
    
        err = esp_partition_erase_range((const esp_partition_t *)new_erase_write_partition, last_erase_offset, 4096);
        if(err != ESP_OK){     
          return err;
        }
        last_erase_offset += 4096;

      }while((offset + data_length) >= last_erase_offset);

  }

  
  err = esp_partition_write((const esp_partition_t *) new_erase_write_partition, offset , data , data_length);
  if(err != ESP_OK){
    CY_LOGI(OTA_DB, "ota_erase_write: %.4X" , (int) err);
  }  
  return err;
}

