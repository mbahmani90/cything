/*
 * Placeholder network spec, so examples/Basic builds out of the box.
 *
 * Every value is the library's own default, which makes this tab a no-op. A
 * real one is generated per channel by the phone app: a device model's
 * "Download firmware sketch" produces a bundle with this file filled in.
 */
#pragma once

#define CYTHING_AP_SSID_PREFIX      "Cy_WiFi_"
#define CYTHING_ROUTER_SSID_PREFIX  "ssid:"
#define CYTHING_ROUTER_PASS_PREFIX  "pass:"
#define CYTHING_TCP_PORT            1234
#define CYTHING_UDP_PORT            1234
#define CYTHING_SCAN_COMMAND        "GET_INFO"
#define CYTHING_SCAN_RESPONSE       "ACK"
#define CYTHING_MULTICAST_IPV4      "232.10.11.12"

/* BLE pairing base UUID: 6A480001-DC02-4C78-A1A3-64193A12E418
 * The 16 bytes below are that string read backwards - the little-endian
 * order NimBLE takes - with the 16-bit service slot left 0x00, 0x00 at
 * indices 12 and 13. The library fills the slot per service. */
#define CYTHING_BLE_BASE_UUID128_BYTES \
    0x18, 0xE4, 0x12, 0x3A, 0x19, 0x64, 0xA3, 0xA1, 0x78, 0x4C, 0x02, 0xDC, 0x00, 0x00, 0x48, 0x6A
