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

/* pvParameters: address family (AF_INET). */
void tcp_server_task(void *pvParameters);
