#pragma once

#define TAG "Cy"

#define HARDWARE_VERSION "1.0"
#define FIRMWARE_VERSION "1.0.1"

#define SET_ROUTER_SSID_PREFIX "ssid:"
#define SET_ROUTER_SSID_SUFFIX "\n"
#define SET_ROUTER_PASS_PREFIX "pass:"
#define SET_ROUTER_PASS_SUFFIX "\n"


#define TCP_PORT    1234
#define UDP_PORT    1234

/* Default IPv4 multicast group for UDP discovery. The effective value is
 * cy_multicast_ipv4 (device_config/cy_config.h); this is its weak default. */
#define MULTICAST_IPV4_ADDR "232.10.11.12"

#define SCAN_COMMAND     "GET_INFO"
#define SCAN_COMMAND_LEN (sizeof(SCAN_COMMAND) - 3)

#define AWS_SCAN_RESPONSE     "ACK"
#define AWS_SCAN_RESPONSE_LEN (sizeof(AWS_SCAN_RESPONSE) - 1)

#define DEVICE_NAME   "dev1"        // Device name
#define DEVICE_TYPE   "devtype1"    // Must be lowercase: Device type in phone app

/* Local-link access policy (security/access_policy.h). 0: every TCP line is
 * accepted as before and the password handshake is merely available — run
 * this until the app that pairs is rolled out. 1: only GET_INFO, the
 * handshake and app_command_is_public() lines work without pairing; every
 * other line needs a paired phone and must arrive encrypted; SEND_TO_ALL
 * reaches paired sockets only. */
#define LOCAL_AUTH_ENFORCE 1
