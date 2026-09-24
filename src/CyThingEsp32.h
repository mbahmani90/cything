#pragma once

/*
 * CyThing for ESP32 — public API of the firmware-as-a-library. This is the
 * only header an ESP32 application needs: call cything_begin() once at boot,
 * then implement the two command hooks. (Per-platform headers: this one is
 * the ESP32 build; other MCUs get their own CyThing<Mcu>.h.) Everything is declared with C linkage so a C++ sketch
 * (.ino / main.cpp) can define the hooks with a plain function definition and
 * still override the weak C defaults. See doc/cy_thing_lib.md.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SEND_TO_ALL, send_data_to_clients(), send_raw_to_client(). Included inside
 * the extern "C" block so a C++ sketch links against the C symbols. */
#include "tcp_server/tcp_client_list.h"

/* The channel spec / model identity symbols the application may override
 * (cything_config.cpp / .ino). Inside the extern "C" block for the same reason,
 * and load-bearing for a second one: in C++ a namespace-scope `const` has
 * INTERNAL linkage without a prior extern declaration, so an override written
 * without this header in scope is silently ignored. */
#include "device_config/cy_config.h"

/* Brings up the whole firmware: response FIFOs, NVS, provisioning, boot
 * partition, Wi-Fi (STA or pairing AP) and the TCP/UDP/MQTT servers. Returns
 * once the tasks are started; everything then runs in FreeRTOS tasks. Call
 * once — from app_main() under ESP-IDF, from setup() under Arduino. */
void cything_begin(void);

/* ---- Command hooks — implemented by the application ------------------
 *
 * Both are weak no-ops inside the library; define them (no attributes needed)
 * to handle your own commands. Both run on library tasks, not on loop(): keep
 * them short and non-blocking, no delay(). */

/* TCP line that no built-in handler (provisioning, OTA, pairing) claimed.
 * Device commands belong to the application; the library defines none. `line` is NUL-terminated with its trailing '\n' still
 * attached; `len` excludes the '\n'; `sock` is the client it came from.
 * Reply with send_data_to_clients(SEND_TO_ALL, ...) to broadcast to every TCP
 * client and MQTT, or send_raw_to_client(sock, ...) to answer only the sender.
 * Return true once handled; false lets the line be logged as unrecognised. */
bool app_command_handle_line(const char *line , int len , int sock);

/* Local-link access policy (doc/local-auth.md). With LOCAL_AUTH_ENFORCE on,
 * a TCP line is only executed for a phone that has paired with the device
 * password, and must arrive encrypted. Return true here for commands that
 * should work for ANYONE on the network with no pairing — a harmless
 * read-out, say. Everything else (including
 * OTA and provisioning lines) stays protected. GET_INFO and the handshake
 * lines are public regardless. Weak no-op default: nothing is public. */
bool app_command_is_public(const char *line , int len);

/* Payload received on the device's MQTT command topic. `command` is NOT
 * NUL-terminated — always use `command_length`. Reply with
 * mqtt_publish_response() or send_data_to_clients(SEND_TO_ALL, ...). Return
 * true once handled; on false the default "Hi I'm ESP32 Smart Device: Remote"
 * reply is published. */
bool app_mqtt_command_handle(const char *command , int command_length);

/* Publish "<id>:<text>" on the MQTT response topic right now, with a fresh
 * response id. Only valid inside app_mqtt_command_handle() (it needs the live
 * MQTT context, which belongs to aws_iot_task); elsewhere it logs a warning
 * and drops the message. */
void mqtt_publish_response(const char *text);

/* ---- Optional: per-model claim certificate + key --------------------
 *
 * Proves "genuine unit of this model" during provisioning (the device
 * answers REQID:<id> with CLAIM:<cert>,<RSA-SHA256 sig>). The library ships
 * EMPTY weak defaults, which skip the claim step. To enable it, define both
 * in the application as NUL-terminated PEM strings. The example sketch does
 * it with two raw-string files the PEMs are pasted into verbatim:
 *
 *   // claim_credentials.ino
 *   const char claim_cert_pem[] =
 *   #include "claim_cert.pem.h"
 *   ;
 *
 *   // claim_cert.pem.h
 *   R"PEM(
 *   -----BEGIN CERTIFICATE-----
 *   ...
 *   -----END CERTIFICATE-----
 *   )PEM"
 *
 * These declarations are what make that work from C++: without a prior
 * extern declaration a namespace-scope const has internal linkage and would
 * never reach the linker. Keep the real PEMs out of version control. */
extern const char claim_cert_pem[];
extern const char claim_key_pem[];

#ifdef __cplusplus
}
#endif
