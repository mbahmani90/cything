#pragma once

#include <stdint.h>
#include "ble/ble_nimble.h"   /* ble_uuid128_t */

/*
 * Shared NimBLE bring-up for the device's two BLE modes, which never run at
 * the same time:
 *   - pairing (soft-AP) mode: ble_pairing.c, a connectable GATT server;
 *   - station mode:           ble_beacon.c, a non-connectable scan beacon.
 * Each mode sets its own ble_hs_cfg callbacks / services / name between
 * ble_host_init() and ble_host_run(), and calls ble_host_resolve_addr() from
 * its sync callback. Bringing the host up twice in one boot is not supported.
 */

/* Controller + NimBLE host init (BLE only on the dual-mode ESP32; the
 * Classic BT RAM goes back to the heap). Returns 0 on success; on failure
 * the error is logged and the caller just stays without BLE. */
/*
 * Fills `out` with the channel's BLE base UUID (cy_ble_base_uuid128) carrying
 * `id` in the 16-bit service slot — the runtime form of what the
 * BLE_PAIRING_UUID128_BYTES(id) macro used to do at compile time. The base is
 * per channel now (the app exports it), so the UUIDs cannot be static
 * initializers any more; call this before registering services or advertising.
 */
void ble_uuid128_from_base(ble_uuid128_t *out, uint16_t id);

int ble_host_init(void);

/* Start the host task. Call once the mode's ble_hs_cfg callbacks, services
 * and device name are in place; the mode's sync_cb then runs on the host
 * task once the controller is up. */
void ble_host_run(void);

/* From the mode's sync_cb: make sure the controller has a usable address and
 * pick the type to advertise with. Returns 0 on success. */
int ble_host_resolve_addr(uint8_t *own_addr_type);

/* One line with the free heap once a mode is up. */
void ble_host_log_started(const char *mode, const char *name);
