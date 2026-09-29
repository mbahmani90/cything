#pragma once

#include <stddef.h>


/* Max size of one UDP reply. The GET_INFO scan reply is ~200 bytes worst
 * case (IP + two 40-byte ids + name/type/versions + commas). */
#define UDP_REPLY_MAX 512

int  processData(char *rx_buffer , int len , char *reply , size_t reply_size);
void udp_server_task(void *pvParameters);
