#pragma once

#include <stddef.h>

void get_device_wifi_info(char *device_wifi_info);
/* Formats the GET_INFO scan reply into `reply`; returns its length. */
int  udp_get_info_response(char *reply , size_t reply_size);