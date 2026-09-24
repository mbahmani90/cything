#pragma once

#include "esp_err.h"


#define FLASH_BOOT_PARTITION_KEY      "B"
#define FLASH_BOOT_PARTITION_KEY_SIZE  0x01
#define FLASH_BOOT_PARTITION_OFFSET    0x00
#define FLASH_BOOT_PARTITION_SIZE      0x02

esp_err_t flash_read_boot_partition();
esp_err_t flash_erase_boot_partition();
esp_err_t flash_erase_write_boot_partition(char boot_partition);