/*
 * Per-channel / per-model configuration for the Arduino build.
 *
 * Nothing to edit: every value comes from the two generated tabs beside this
 * one, cything_network_spec.h and cything_device_params.h. They and this file
 * all arrive together in the sketch bundle the phone app exports from a device
 * model ("Download firmware sketch"). Delete this tab and the library's weak
 * defaults (src/device_config/cy_config.c) apply instead, i.e. the firmware
 * behaves as it did before these values became configurable.
 *
 * The CyThingEsp32.h include is load-bearing: in C++ a namespace-scope `const`
 * has internal linkage unless a prior extern declaration is in scope, so
 * without it these definitions are silently ignored and the weak defaults win.
 *
 * The ESP-IDF equivalent is main/cything_config.cpp.example, and the app
 * generates its own copy of this file into every bundle. All three carry the
 * same symbol list and must stay in step.
 */
#include <CyThingEsp32.h>
#include "cything_network_spec.h"
#include "cything_device_params.h"

const char     cy_ap_ssid_prefix[]     = CYTHING_AP_SSID_PREFIX;
const char     cy_router_ssid_prefix[] = CYTHING_ROUTER_SSID_PREFIX;
const char     cy_router_pass_prefix[] = CYTHING_ROUTER_PASS_PREFIX;
const uint16_t cy_tcp_port             = CYTHING_TCP_PORT;
const uint16_t cy_udp_port             = CYTHING_UDP_PORT;
const char     cy_scan_command[]       = CYTHING_SCAN_COMMAND;
const char     cy_scan_response[]      = CYTHING_SCAN_RESPONSE;
const char     cy_multicast_ipv4[]     = CYTHING_MULTICAST_IPV4;
const uint8_t  cy_ble_base_uuid128[16] = { CYTHING_BLE_BASE_UUID128_BYTES };

const char cy_device_name[]      = CYTHING_DEVICE_NAME;
const char cy_device_type[]      = CYTHING_DEVICE_TYPE;
const char cy_hardware_version[] = CYTHING_HARDWARE_VERSION;
const char cy_major_unique_id[]  = CYTHING_MAJOR_UNIQUE_ID;
const char cy_ota_username[]     = CYTHING_OTA_USERNAME;
const char cy_ota_password[]     = CYTHING_OTA_PASSWORD;
const char cy_initial_password[] = CYTHING_INITIAL_PASSWORD;

/* The firmware repo owns the version string (scripts/release.sh rewrites
 * FIRMWARE_VERSION). Uncomment to let the model's value win instead:
 * const char cy_firmware_version[] = CYTHING_FIRMWARE_VERSION; */
