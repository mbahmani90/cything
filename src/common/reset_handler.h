#pragma once

#include <stdint.h>

void reset_handler();

/* Reboot once `ms` have elapsed, from the esp_timer task; the caller returns
 * immediately. Use this instead of a "vTaskDelay then esp_restart" task so
 * a reply already queued on the TCP send task has time to leave. Calling it
 * again just restarts the countdown. */
void reboot_after_ms(uint32_t ms);
