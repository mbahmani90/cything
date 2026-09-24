#include "tcp_response_fifo.h"

response_fifo_t tcp_response_fifo;

void tcp_response_fifo_init(void){
    response_fifo_init(&tcp_response_fifo, "tcp_response");
}
