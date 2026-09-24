#pragma once

/*
 * The NimBLE host headers, for the two BLE modes (ble_pairing.c, ble_beacon.c)
 * and their shared bring-up (ble_host.c). One include block because the paths
 * differ per build: NimBLE-Arduino keeps the host under its own tree and needs
 * nimconfig.h first (it is the syscfg the headers read); ESP-IDF ships the
 * same host as a component with flat include paths.
 */
#include "esp_bt.h"

#ifdef ARDUINO
#include "nimconfig.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/host/include/host/ble_gap.h"
#include "nimble/nimble/host/include/host/ble_gatt.h"
#include "nimble/nimble/host/include/host/ble_uuid.h"
#include "nimble/nimble/host/include/host/ble_hs_mbuf.h"
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/util/include/host/util/util.h"
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include "nimble/porting/nimble/include/nimble/nimble_port.h"
#include "nimble/porting/npl/freertos/include/nimble/nimble_port_freertos.h"
#if defined(CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE)
/* esp_nimble_hci_init(): allocates the NimBLE mbuf pools. nimble_port_init()
 * does NOT do this under NimBLE-Arduino — see ble_host_init(). */
#include "nimble/esp_port/esp-hci/include/esp_nimble_hci.h"
#endif
#else
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#endif
