#pragma once

/*
 * TCP command server on TCP_PORT (device_config.h). tcp_server_task listens,
 * accepts, registers each client in the client list and spawns a
 * tcp_client_recv_task for it. See doc/tcp-server.md.
 *
 * Module layout (main/tcp_server/):
 *   tcp_server.c       - listener task (this file)
 *   tcp_client_recv.c  - per-client receive task + line framing
 *   tcp_command.c      - line dispatcher, HTTPS OTA handshake, local commands
 *   pairing.c          - Wi-Fi pairing state machine + inactivity timer
 *   tcp_client_list.c  - client registry, response FIFO producers, send task
 *   tcp_response_fifo.c - the TCP response FIFO instance
 */

/* Phone connections served at once. Each costs a socket (the device has
 * CONFIG_LWIP_MAX_SOCKETS = 10, shared with the listener, UDP, MQTT/TLS and
 * DNS) and a TCP_SERVER_RECV_TASK_STACK_SIZE task stack, so the cap is what
 * keeps a burst of phone reconnects from exhausting either. At the cap a new
 * connection replaces the same phone's (same IP) older one, or is refused. */
#ifndef TCP_CLIENT_MAX
#define TCP_CLIENT_MAX 3
#endif

/* Keepalive on phone connections: a phone that vanished without closing
 * (Wi-Fi switch, app killed) is dropped after IDLE + INTVL * CNT seconds
 * instead of holding its socket forever. */
#define TCP_KEEPALIVE_IDLE_S   20
#define TCP_KEEPALIVE_INTVL_S  5
#define TCP_KEEPALIVE_CNT      3

/* pvParameters: address family (AF_INET). */
void tcp_server_task(void *pvParameters);
