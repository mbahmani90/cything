#pragma once

#include "esp_netif.h"

/*
 * Station-mode scan beacon (doc/ble-scan-beacon.md). Once the device is on
 * the router it advertises, non-connectably, the BLE_SCAN_SVC_ID service
 * plus its LAN IPv4 in Manufacturer Specific Data, and its identity as the
 * local name in the scan response:
 *
 *   ADV      Flags | 128-bit scan service UUID | 0xFF: company, ver, ip1..ip4
 *   SCAN_RSP Local name = deviceId ("dev_…"), or "Cy_" + unit suffix while
 *            the device is not provisioned yet
 *
 * A phone that hears it sends a unicast GET_INFO to that IP — no BLE
 * connection, no GATT. Unicast crosses access points where the multicast
 * scan query does not, which is the whole point. The UDP reply, not the
 * beacon, is what the app trusts: the beacon only says "try this address".
 *
 * Advertising runs only while the station has an IP: it starts on
 * IP_EVENT_STA_GOT_IP (with the current address — DHCP renewals update it)
 * and stops on WIFI_EVENT_STA_DISCONNECTED, so a stale address is never
 * advertised. All entry points are safe from any task.
 */

/* Bring up the controller + host for the beacon. Call once from
 * cything_begin() in station mode, instead of ble_release_memory(). Nothing
 * is advertised until ble_beacon_on_ip(). Failures are logged; the device
 * then just has no beacon. */
void ble_beacon_start(void);

/* Station got (or renewed) its address: (re)start advertising with it. */
void ble_beacon_on_ip(const esp_ip4_addr_t *ip);

/* Station lost the router: stop advertising. */
void ble_beacon_on_ip_lost(void);

/* Discovery mode changed (DISCOVERYMODE owner command): stop the beacon if
 * the mode no longer includes BLE, (re)start it if it does. No-op until
 * ble_beacon_start() and an IP. */
void ble_beacon_apply_discovery_mode(void);
