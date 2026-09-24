#pragma once

#include "common/response_fifo.h"

/*
 * The local (TCP) response FIFO. Producers: send_data_to_clients() and
 * send_raw_to_client() in tcp_client_list.c. Consumer: tcp_response_send_task,
 * which wakes on every push and sends each line to its target socket(s).
 * Lines are "<sock>:<payload>\n"; see doc/send-buffers.md.
 */

extern response_fifo_t tcp_response_fifo;

/* Call once from app_main() before the TCP server or any producer starts. */
void tcp_response_fifo_init(void);
