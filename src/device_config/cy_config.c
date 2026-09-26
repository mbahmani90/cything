/*
 * Default channel spec and model identity (see device_config/cy_config.h).
 *
 * Every value here is the macro it replaced, so a build with no application
 * override behaves exactly as it did when these were compile-time constants.
 * Weak, so a strong definition in the application — the generated
 * cything_config.cpp / .ino — takes over at link time, exactly like the claim
 * credentials and the command hooks.
 */
#include <string.h>

#include "device_config/cy_config.h"
#include "device_config/device_config.h"
#include "wifi/wifi_config.h"
#include "ble/ble_config.h"
#include "common/cy_log.h"

__attribute__((weak)) const char     cy_ap_ssid_prefix[]     = ESP_AP_WIFI_SSID_PREFIX;
__attribute__((weak)) const char     cy_router_ssid_prefix[] = SET_ROUTER_SSID_PREFIX;
__attribute__((weak)) const char     cy_router_pass_prefix[] = SET_ROUTER_PASS_PREFIX;
__attribute__((weak)) const uint16_t cy_tcp_port             = TCP_PORT;
__attribute__((weak)) const uint16_t cy_udp_port             = UDP_PORT;
__attribute__((weak)) const char     cy_scan_command[]       = SCAN_COMMAND;
__attribute__((weak)) const char     cy_scan_response[]      = AWS_SCAN_RESPONSE;
__attribute__((weak)) const char     cy_multicast_ipv4[]     = MULTICAST_IPV4_ADDR;

/* Slot bytes 12/13 zero: ble_uuid128_from_base() writes the service id there. */
__attribute__((weak)) const uint8_t  cy_ble_base_uuid128[16] = {
    BLE_PAIRING_UUID128_BYTES(0x0000)
};

__attribute__((weak)) const char cy_device_name[]      = DEVICE_NAME;
__attribute__((weak)) const char cy_device_type[]      = DEVICE_TYPE;
__attribute__((weak)) const char cy_hardware_version[] = HARDWARE_VERSION;
__attribute__((weak)) const char cy_firmware_version[] = FIRMWARE_VERSION;
__attribute__((weak)) const char cy_major_unique_id[]  = "";
__attribute__((weak)) const char cy_ota_username[]     = "";
__attribute__((weak)) const char cy_ota_password[]     = "";
__attribute__((weak)) const char cy_initial_password[] = "12345678";

size_t cy_scan_command_len(void){
    /* == sizeof(SCAN_COMMAND) - 3 for the default "GET_INFO". */
    size_t length = strlen(cy_scan_command);
    return length > 2 ? length - 2 : length;
}

void cy_config_log(void){
    CY_LOGI(1, "config: tcp=%u udp=%u scan='%s' resp='%s' ap='%s' mcast=%s",
            (unsigned)cy_tcp_port, (unsigned)cy_udp_port,
            cy_scan_command, cy_scan_response, cy_ap_ssid_prefix, cy_multicast_ipv4);
    CY_LOGI(1, "config: device='%s' type='%s' hw=%s fw=%s ble=%02X%02X....%02X%02X",
            cy_device_name, cy_device_type, cy_hardware_version, cy_firmware_version,
            cy_ble_base_uuid128[15], cy_ble_base_uuid128[14],
            cy_ble_base_uuid128[1],  cy_ble_base_uuid128[0]);
}
