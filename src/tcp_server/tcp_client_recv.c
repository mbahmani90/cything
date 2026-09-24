#include "tcp_client_recv.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "common/cy_log.h"
#include "security/local_session.h"
#include "security/enc_frame.h"
#include "tcp_server/tcp_client_list.h"
#include "tcp_server/tcp_command.h"

void tcp_client_recv_task(void *pvParameters){

    char temp_rx_buffer[5];
    char rx_buffer[TCP_RECEIVE_DATA_LENGTH];

    local_session_t *session = (local_session_t *) pvParameters;
    const int sock = session->sock;

    session->rx_len = 0;

    // Inter-byte receive timeout: if a message stalls mid-way, recv() returns
    // -1 / EWOULDBLOCK and the partial message is discarded below.
    struct timeval rcv_timeout = {
        .tv_sec  = TCP_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec = (TCP_RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    while (1) {
        int len = recv(sock, temp_rx_buffer, 1 , 0);

        // Error occured during receiving
        if (len < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Receive timeout: drop any partial message, keep the connection.
                if (session->rx_len > 0) {
                    CY_LOGW(TCP_SERVER_DB, "recv timeout, discarding %d partial bytes", session->rx_len);
                }
                session->rx_len = 0;
                continue;
            }
            CY_LOGE(TCP_SERVER_DB, "recv failed: errno %d", errno);
            break;
        }
        // Connection closed
        else if (len == 0) {
            CY_LOGI(TCP_SERVER_DB, "Connection closed");
            break;
        }
        // Data received
        else {
            rx_buffer[session->rx_len++] = temp_rx_buffer[0];

            if(session->rx_len >= TCP_RECEIVE_DATA_LENGTH){
                // Oversized line: drop it and resync on the next '\n'.
                session->rx_len = 0;
            }
            else if(session->rx_len >= TCP_RECEIVE_DATA_SUFFIX_LENGTH &&
                    memcmp(rx_buffer + session->rx_len - TCP_RECEIVE_DATA_SUFFIX_LENGTH,
                           TCP_RECEIVE_DATA_SUFFIX, TCP_RECEIVE_DATA_SUFFIX_LENGTH) == 0){

                rx_buffer[session->rx_len] = 0;
                int line_len = session->rx_len - TCP_RECEIVE_DATA_SUFFIX_LENGTH;
                session->rx_len = 0;
                // ENC: frames are decrypted in place first; a bad frame is dropped here.
                if(enc_frame_unwrap(rx_buffer, &line_len, sock, sizeof(rx_buffer))){
                    CY_LOGI(TCP_SERVER_DB, "%s   %d  %d\n", rx_buffer , sizeof(rx_buffer) , line_len);
                    tcp_dispatch_line(rx_buffer , line_len , sock);
                }
                CY_LOGI(TCP_SERVER_DB, "tcp_client_recv_task stack high-water mark: %u bytes free",
                        (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
    CY_LOGE(TCP_SERVER_DB, "Shutting down socket and restarting...");

    // Release the session before the registry entry: once the slot is gone
    // no reply can be encrypted for this socket, and the send task drops
    // anything still queued once the account is removed.
    local_session_release(sock);
    remove_account(sock);

    shutdown(sock, 0);
    close(sock);

    vTaskDelete(NULL);
}
