#pragma once

#include "esp_event.h"
#include "esp_netif.h"

/* The soft-AP interface, valid after wifi_init_accesspoint_mode(). */
extern esp_netif_t *p_netif_ap;

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void wifi_init_accesspoint_mode();
