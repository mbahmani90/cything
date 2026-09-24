#pragma once

#include <stddef.h>


/* Max size of one UDP reply. The GET_INFO scan reply is ~200 bytes worst
 * case (IP + two 40-byte ids + name/type/versions + commas). */
#define UDP_REPLY_MAX 512

int  processData(char *rx_buffer , int len , char *reply , size_t reply_size);
void udp_server_task(void *pvParameters);

/* Discovery mode changed: the server rebuilds its socket (within ~1 s) so
 * the multicast group membership matches discovery_mode_should_scan_wifi().
 * Safe from any task. */
void udp_server_reconfigure(void);
