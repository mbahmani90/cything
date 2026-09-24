#pragma once

#include <stdint.h>

/*
 * Wi-Fi pairing over BLE. While the device is in pairing (soft-AP) mode it
 * also advertises a GATT service the phone writes the router SSID and
 * password into. COMMIT first *tries* the credentials (wifi/wifi_trial.c:
 * APSTA, wait for an IP) and reports the outcome on Status; only when the
 * router handed out an address are they stored and the device reboots into
 * station mode — the same end as the TCP flow (tcp_server/pairing.c,
 * pairing_commit()), which still commits blind. UUIDs and tunables live in
 * ble_config.h. See doc/ble-pairing.md.
 *
 * Protocol (all values little-endian / raw bytes, no framing):
 *   SSID      write   1..32 bytes, UTF-8, not NUL-terminated
 *   Password  write   0..64 bytes ("" for an open network)
 *   Control   write   1 byte, BLE_PAIR_CTRL_*
 *   Status    read/notify  2 bytes: [BLE_PAIR_STATE_*, detail]
 *   Info      read/notify  the GET_INFO scan CSV (udp_response_handler.c):
 *                          ip,sourceTerminalId,deviceName,deviceType,
 *                          deviceId,provisionState,fw,hw,caps. Identity is
 *                          valid any time; `ip` is the router-assigned
 *                          station address once Status is WIFI_JOINED
 *                          (before that: the soft-AP's own address).
 */

/* Control opcodes. */
#define BLE_PAIR_CTRL_COMMIT      0x01  /* store SSID + password, reboot into STA */

/* Status byte 0. */
#define BLE_PAIR_STATE_IDLE       0x00  /* no SSID received yet */
#define BLE_PAIR_STATE_READY      0x01  /* SSID received; waiting for COMMIT */
#define BLE_PAIR_STATE_STORED     0x02  /* credentials in flash; rebooting in 1 s */
#define BLE_PAIR_STATE_TRYING     0x03  /* COMMIT accepted; connecting to the router */
#define BLE_PAIR_STATE_WIFI_JOINED 0x04 /* router gave us an IP — read Info; STORED follows */
#define BLE_PAIR_STATE_ERROR      0xFF  /* byte 1 = BLE_PAIR_ERR_* */

/* Status byte 1 when byte 0 is BLE_PAIR_STATE_ERROR. */
#define BLE_PAIR_ERR_NO_SSID      0x01  /* COMMIT before an SSID was written */
#define BLE_PAIR_ERR_FLASH        0x02  /* wifi_info partition write failed */
#define BLE_PAIR_ERR_BAD_OPCODE   0x03  /* unknown Control byte */
#define BLE_PAIR_ERR_BUSY         0x04  /* a commit is already in progress */
#define BLE_PAIR_ERR_WIFI_NOT_FOUND 0x05 /* no AP with that SSID in range; nothing stored */
#define BLE_PAIR_ERR_WIFI_AUTH    0x06  /* router refused the password; nothing stored */
#define BLE_PAIR_ERR_WIFI_TIMEOUT 0x07  /* no IP within BLE_PAIRING_WIFI_TRIAL_TIMEOUT_MS; nothing stored */

/* Size limits: 802.11 SSID max, WPA2 passphrase max (+1 for a 64-hex PSK
 * is not supported by the station code either). */
#define BLE_PAIR_SSID_MAX         32
#define BLE_PAIR_PASS_MAX         64

/* Bring up the BLE controller + NimBLE host, register the pairing and
 * Device Information services and start advertising. Call once, from
 * cything_begin(), only in pairing (AP) mode. Failures are logged; the
 * device then simply stays pairable over TCP only. */
void ble_pairing_start(void);

/* Station mode never uses Bluetooth: hand the controller + host RAM
 * (~60 KB on ESP32) back to the heap. Call once at boot instead of
 * ble_pairing_start(). */
void ble_release_memory(void);
