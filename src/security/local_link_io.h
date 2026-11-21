#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Where the local-link security code (pake_handler.c, local_auth.c,
 * enc_frame.c) sends its bytes. A TCP client is a socket; a phone on the
 * station-mode BLE GATT server (ble/ble_app_gatt.c) gets a pseudo-socket
 * number from LOCAL_LINK_BLE_SOCK_BASE up, so the same session pool, PAKE,
 * ENROLL, AUTH and ENC: framing serve both links (doc/ble-gatt-server.md).
 */

/* Above any lwIP socket number; one per BLE connection handle. */
#define LOCAL_LINK_BLE_SOCK_BASE 0x4000
#define LOCAL_LINK_BLE_SOCK_SPAN 0x1000

/* The "peer IP" BLE sessions are rate limited under (device_password.c):
 * one bucket for every BLE phone, never a TCP peer's address (and not 0,
 * which marks a free slot there). The global limit applies as on TCP. */
#define LOCAL_LINK_BLE_PEER_IP 0xFFFFFFFFu

static inline bool local_link_is_ble(int sock){
    return sock >= LOCAL_LINK_BLE_SOCK_BASE && sock < LOCAL_LINK_BLE_SOCK_BASE + LOCAL_LINK_BLE_SOCK_SPAN;
}

/* send() for a TCP socket; queued for the phone to read for a BLE one. */
void local_link_send(int sock, const void *data, size_t len);
