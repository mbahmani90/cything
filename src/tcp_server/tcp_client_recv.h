#pragma once

/*
 * Per-connection receive task: one tcp_client_recv_task per accepted socket.
 * Reads byte by byte, frames on TCP_RECEIVE_DATA_SUFFIX ("\n") and hands
 * each complete line to tcp_dispatch_line(). See doc/tcp-server.md.
 *
 * Framing state lives in the connection's local_session_t (rx_len), so two
 * clients typing at once no longer clobber each other's byte count.
 */

#undef  CONSTANT_TCP_RECEIVE_LEN
#define TCP_RECEIVE_DATA_LENGTH        5120  // max line incl. "\n"; lives on the client task stack
#define TCP_RECEIVE_DATA_SUFFIX        "\n"
#define TCP_RECEIVE_DATA_SUFFIX_LENGTH  1
#define TCP_RECEIVE_TIMEOUT_MS         100   // inter-byte timeout for a partial message

/* pvParameters: local_session_t * for this client (security/local_session.h). */
void tcp_client_recv_task(void *pvParameters);
