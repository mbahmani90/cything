#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

/*
 * Stack size and priority of every FreeRTOS task the firmware creates.
 * One place to tune; see doc/tasks.md for what each task does and how the
 * sizes were chosen.
 *
 * Stack sizes are in BYTES (ESP-IDF semantics). Priorities: higher number =
 * higher priority; 0 is the idle priority (configMAX_PRIORITIES is 25).
 */

/* Long-running tasks -------------------------------------------------- */

/* Wi-Fi station bring-up; one-shot but listed here because it is large. */
#define WIFI_STA_TASK_STACK_SIZE          4096
#define WIFI_STA_TASK_PRIORITY            5

/* TCP listener (accept loop). */
#define TCP_SERVER_TASK_STACK_SIZE        4096
#define TCP_SERVER_TASK_PRIORITY          5

/* One per connected TCP client (tcp_client_recv_task).
 * rx_buffer[TCP_RECEIVE_DATA_LENGTH] is a stack local, so this must be
 * TCP_RECEIVE_DATA_LENGTH + ~3.5 KB for the deepest call chain (provisioning
 * / mbedTLS). 5120 + 3.5 KB ~= 8.6 KB -> 10240 leaves ~1.6 KB margin. */
#define TCP_SERVER_RECV_TASK_STACK_SIZE   10240
#define TCP_SERVER_RECV_TASK_PRIORITY     5

/* Local response sender: drains tcp_response_fifo to every TCP client as
 * soon as a line is pushed. Staging buffer is static; the stack holds the
 * send() call chain plus enc_frame_send()'s ~1.9 KB of frame buffers and
 * GCM context for sockets with a session key. */
#define TCP_RESPONSE_SEND_TASK_STACK_SIZE 5120
#define TCP_RESPONSE_SEND_TASK_PRIORITY   5

/* UDP discovery server (GET_INFO only; pairing is TCP).
 * Deepest path recvfrom -> processData -> udp_get_info_response holds
 * ~820 B of buffers (rx/addr/reply[UDP_REPLY_MAX]) + ~1 KB for CY_LOGI's
 * printf. It logs its high-water mark after every packet; re-tune from a
 * measured figure, not by guessing. */
#define UDP_SERVER_TASK_STACK_SIZE        4096
#define UDP_SERVER_TASK_PRIORITY          5

/* MQTT client (coreMQTT + mbedTLS).
 * NOTE: priority 0 is the idle priority and 4096 bytes is small for a TLS
 * handshake — kept as-is for now, see doc/tasks.md "Observations". */
#define AWS_IOT_TASK_STACK_SIZE           4096
#define AWS_IOT_TASK_PRIORITY             0

/* One-shot helpers ------------------------------------------------------ */

/* HTTPS OTA download + reboot. */
#define OTA_TASK_STACK_SIZE               8192
#define OTA_TASK_PRIORITY                 5

/* BLE pairing COMMIT: erases + rewrites the wifi_info partition, sends the
 * final status notify and arms the reboot. Runs off the NimBLE host task so
 * the stack never blocks on flash; the partition write is the deep part. */
#define BLE_COMMIT_TASK_STACK_SIZE        4096
#define BLE_COMMIT_TASK_PRIORITY          5

#endif /* TASK_CONFIG_H */
