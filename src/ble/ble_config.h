#pragma once

/* BLE Wi-Fi pairing settings. Everything here is per-product, not per-unit:
 * the same UUIDs and name prefix ship in every device so the mobile app can
 * filter on them. See doc/ble-pairing.md. */

/* Local name in the scan response: prefix + the same 6-hex-digit unit suffix
 * as the soft-AP SSID (wifi_get_unit_suffix), e.g. "Cy_15753C". Keep the
 * prefix short — the whole name must fit the 31-byte scan response. */
#define BLE_PAIRING_NAME_PREFIX "Cy_"

/* Custom 128-bit base UUID 6A48xxxx-DC02-4C78-A1A3-64193A12E418; the
 * 16-bit `xxxx` slot selects the service or characteristic (below). The
 * macro expands to the 16 bytes in the little-endian order NimBLE's
 * BLE_UUID128_INIT() takes (i.e. the string read backwards). Bump the
 * service id (…0001 -> …0101) if the protocol ever changes incompatibly,
 * so old apps stop finding new devices. */
#define BLE_PAIRING_UUID128_BYTES(id) \
    0x18, 0xE4, 0x12, 0x3A, 0x19, 0x64, 0xA3, 0xA1, 0x78, 0x4C, 0x02, 0xDC, \
    (uint8_t)((id) & 0xFF), (uint8_t)(((id) >> 8) & 0xFF), 0x48, 0x6A

#define BLE_PAIRING_SVC_ID      0x0001   /* Wi-Fi pairing service            */
#define BLE_PAIRING_SSID_ID     0x0002   /* write: router SSID, <= 32 bytes  */
#define BLE_PAIRING_PASS_ID     0x0003   /* write: router password, <= 64 B  */
#define BLE_PAIRING_CTRL_ID     0x0004   /* write: 1-byte opcode             */
#define BLE_PAIRING_STATUS_ID   0x0005   /* read + notify: [state, detail]   */
#define BLE_PAIRING_INFO_ID     0x0006   /* read + notify: GET_INFO scan CSV */

/* Writes to SSID / password / control need an encrypted link (LE Secure
 * Connections "Just Works", no PIN, no bonding). The phone pairs
 * automatically the first time it writes; the user sees one OS dialog. Set
 * to 0 to accept plain-text writes (debugging, or an app that cannot pair). */
#define BLE_PAIRING_REQUIRE_ENCRYPTION 1

/* Advertising interval, ms. 100-200 ms is the usual "discoverable" range:
 * found within a second or two, without hammering the 2.4 GHz band the
 * soft-AP shares. */
#define BLE_PAIRING_ADV_INTERVAL_MIN_MS 100
#define BLE_PAIRING_ADV_INTERVAL_MAX_MS 200

/* How long COMMIT waits for the router to hand out an IP before reporting
 * BLE_PAIR_ERR_WIFI_TIMEOUT. Association + DHCP is usually 2-5 s; slow
 * routers and band-steering can take longer. */
#define BLE_PAIRING_WIFI_TRIAL_TIMEOUT_MS 15000

/* ------------------------------------------------ station-mode beacon --- */

/* In station mode the device keeps BLE up as a non-connectable *scan
 * beacon* (ble_beacon.c, doc/ble-scan-beacon.md): the app filters its scan
 * on BLE_SCAN_SVC_ID and reads the device's LAN IP straight out of the
 * advertising packet, then confirms identity with a unicast GET_INFO to that
 * IP. Set to 0 to drop station-mode BLE entirely and give its RAM (~60 KB)
 * back to the heap, as before. */
#ifndef BLE_SCAN_BEACON_ENABLED
#define BLE_SCAN_BEACON_ENABLED 1
#endif

/* Service id in the same base UUID as the pairing service — a *different*
 * slot so a phone scanning for pairable units never lists paired ones and
 * vice versa. The app derives it from the channel's bleServiceUuid the same
 * way it derives characteristic UUIDs. */
#define BLE_SCAN_SVC_ID         0x0101

/* Manufacturer Specific Data in the advertising packet:
 *   company id (2 B, little-endian) | payload version (1 B) | IPv4 (4 B)
 * 0xFFFF is the SIG's "reserved for internal use / testing" company id — we
 * have no assigned one; the service UUID beside it is what identifies us.
 * Bump the version when the layout changes; the app ignores versions it
 * does not know. */
#define BLE_BEACON_COMPANY_ID   0xFFFF
#define BLE_BEACON_PAYLOAD_VER  0x01

/* Beacon advertising interval, ms. Wider than pairing's: the beacon runs for
 * the device's whole uptime beside Wi-Fi (same 2.4 GHz radio, time-sliced),
 * and a phone scanning in the foreground still hears a 200-300 ms beacon
 * within a second. The spec floor for non-connectable advertising on a
 * 4.x controller is 100 ms. */
#define BLE_BEACON_ADV_INTERVAL_MIN_MS 200
#define BLE_BEACON_ADV_INTERVAL_MAX_MS 300
