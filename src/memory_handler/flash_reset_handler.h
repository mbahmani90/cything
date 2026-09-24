#pragma once

#include "esp_err.h"

// #define DISCOVERABLE_OFFSET    0
// #define DISCOVERABLE_SIZE      0x02
// #define DISCOVERABLE_KEY       "D"
// #define DISCOVERABLE_KEY_SIZE  1
#define RESET_COUNTER_OFFSET   0
#define RESET_COUNTER_SIZE     0x02
#define RESET_COUNTER_KEY      "C"
#define RESET_COUNTER_KEY_SIZE 1

extern int registeration_reset_counter;

// esp_err_t flash_read_discoverable();
// esp_err_t flash_erase_write_discoverable(int new_discoverable_mode);
esp_err_t flash_read_reset_counter();
esp_err_t flash_erase_write_reset_counter(int new_reset_counter);