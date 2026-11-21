#pragma once

/*
 * Station-mode GATT server: the channel's own BLE services and
 * characteristics, served once the device is on the router
 * (doc/ble-gatt-server.md).
 *
 * The table comes from the sketch. The phone app generates it into
 * cything_network_spec.h (static arrays named CYTHING_BLE_SERVICES), and the
 * generated cything_config.ino / cything_config.cpp hands it to the library
 * through two symbols:
 *
 *   const cything_ble_svc_t *const cy_ble_services     = CYTHING_BLE_SERVICES;
 *   const uint8_t                  cy_ble_service_count = CYTHING_BLE_SERVICE_COUNT;
 *
 * The library ships weak defaults (NULL, 0). With zero services station mode
 * stays exactly as before: a non-connectable scan beacon and no GATT RAM.
 *
 * Public: CyThingEsp32.h includes this header inside its extern "C" block.
 * Keep it free of NimBLE headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the table ------------------------------------------------------- */

/* cything_ble_chr_t.flags, OR-ed together. */
#define CYTHING_BLE_READ    0x01   /* phone reads  -> app_ble_read()       */
#define CYTHING_BLE_WRITE   0x02   /* phone writes -> app_ble_write()      */
#define CYTHING_BLE_NOTIFY  0x04   /* device pushes via cything_ble_notify() */

/* What the phone app lets a channel define. Larger tables are cut, with an
 * error in the log. */
#define CYTHING_BLE_MAX_SERVICES          16
#define CYTHING_BLE_MAX_CHRS_PER_SERVICE  32

/* Largest value one read / write carries (the ATT attribute limit). */
#define CYTHING_BLE_VALUE_MAX  512

typedef struct {
    /* Unique across the whole table: the hooks and cything_ble_notify()
     * find a characteristic by this name. */
    const char *name;
    /* Full 128-bit UUID, NimBLE little-endian order (the UUID string read
     * backwards, as BLE_UUID128_INIT takes it). A UUID on the Bluetooth base
     * 0000xxxx-0000-1000-8000-00805F9B34FB is registered as the 16-bit xxxx. */
    uint8_t     uuid128[16];
    uint8_t     flags;     /* CYTHING_BLE_READ | _WRITE | _NOTIFY */
} cything_ble_chr_t;

typedef struct {
    uint8_t                  uuid128[16];   /* same byte order as above */
    const cything_ble_chr_t *chrs;
    uint8_t                  chr_count;
} cything_ble_svc_t;

/* Provided by the sketch (generated); weak NULL / 0 in the library. */
extern const cything_ble_svc_t *const cy_ble_services;
extern const uint8_t                  cy_ble_service_count;

/* ---- notify ---------------------------------------------------------- */

#define CYTHING_BLE_ERR_NOT_RUNNING    (-1)  /* no table, pairing mode, or BLE off   */
#define CYTHING_BLE_ERR_UNKNOWN_CHR    (-2)  /* no characteristic by that name       */
#define CYTHING_BLE_ERR_NOT_NOTIFY     (-3)  /* it has no CYTHING_BLE_NOTIFY flag    */
#define CYTHING_BLE_ERR_NO_SUBSCRIBER  (-4)  /* nobody connected + subscribed (+authenticated) */
#define CYTHING_BLE_ERR_NO_MEM         (-5)  /* out of NimBLE buffers, try later     */
#define CYTHING_BLE_ERR_ARG            (-6)  /* NULL name / data, len < 0 or > CYTHING_BLE_VALUE_MAX */

/* Send `data` as a notification on characteristic `name` to every connected
 * phone that subscribed to it and may see it (doc/ble-gatt-server.md,
 * "Security"). A notification carries at most MTU-3 bytes; longer data is
 * cut per phone. Safe from any task, including loop() and the hooks.
 * Returns the number of phones notified (>= 1), or a CYTHING_BLE_ERR_*. */
int cything_ble_notify(const char *name, const uint8_t *data, int len);

/* ATT error codes the server answers with (the phone sees them as a failed
 * read / write). 0x80-0x9F is the application range of the ATT spec. */
#define CYTHING_BLE_ATT_ERR_AUTH      0x80   /* not authenticated: run AUTH on the auth characteristic */
#define CYTHING_BLE_ATT_ERR_REJECTED  0x81   /* app_ble_write() returned false */
#define CYTHING_BLE_ATT_ERR_BUSY      0x82   /* auth link: the previous line is still being handled */

/* ---- library internals (not for sketches) ---------------------------- */

struct ble_gap_event;

/* Number of services the server will register: cy_ble_service_count capped
 * at CYTHING_BLE_MAX_SERVICES, 0 when the table is missing. */
int  ble_app_gatt_service_count(void);

/* Between ble_host_init() and ble_host_run(): build the NimBLE service
 * definitions from the table, add the auth service, register both, and set
 * the security manager up. Returns 0 on success; on failure nothing is
 * registered and the caller stays a plain beacon. */
int  ble_app_gatt_register(void);

/* True once ble_app_gatt_register() succeeded. */
bool ble_app_gatt_running(void);

/* The beacon's GAP callback forwards every event here (connect, disconnect,
 * subscribe, encryption, MTU, repeat pairing). Returns the value the GAP
 * callback must return for this event (only REPEAT_PAIRING needs one). */
int  ble_app_gatt_gap_event(struct ble_gap_event *event);

/* security/local_link_io.c: bytes the pairing code sends to a BLE
 * pseudo-socket, queued for the phone to read on the auth characteristic. */
void ble_app_gatt_link_send(int sock, const void *data, size_t len);

/* True while another phone may still connect. */
bool ble_app_gatt_conn_slot_free(void);
