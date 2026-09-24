#pragma once

#include <stddef.h>
#include "esp_err.h"
extern int last_erase_offset;

void ota_read_otadata_partition();
void ota_read_ota_1_partition();
esp_err_t ota_set_boot_firmware(int partition);
void write_info_partition();
void read_info_partition();

void ota_set_new_write_partition(size_t type , size_t subtype);
void ota_set_new_erase_partition(size_t type , size_t subtype);
void ota_set_new_read_partition (size_t type , size_t subtype);
void ota_set_new_erase_write_partition(size_t type , size_t subtype);

esp_err_t ota_write(long int offset , char *data , long int data_length);
esp_err_t ota_erase(size_t   offset , size_t  length);
esp_err_t ota_read (long int offset , char *data , long int data_length);
esp_err_t ota_erase_write(long int offset , char *data , long int data_length);