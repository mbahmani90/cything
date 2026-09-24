/*
 * ESP-IDF application. Everything below the includes is what a CyThing user
 * writes: the two command hooks and a call to cything_begin(). The Arduino /
 * PlatformIO equivalent is examples/Basic/Basic.ino — see doc/cy_thing_lib.md.
 */
#include <string.h>
#include "CyThingEsp32.h"

/* Application TCP commands. Called for every received line that none of the
 * built-in handlers (provisioning, OTA, pairing, on_cmd/off_cmd) claimed.
 *
 *   line  NUL-terminated, trailing '\n' still attached
 *   len   length excluding the '\n'
 *   sock  the client socket the line came from
 *
 * Reply with send_data_to_clients(SEND_TO_ALL, ...) to broadcast to every TCP
 * client (and MQTT), or send_raw_to_client(sock, ...) to answer only the
 * sender. Return true once the line is handled, false to let it be logged as
 * unrecognised. Add new commands below. */
bool app_command_handle_line(const char *line , int len , int sock)
{
    (void)len;
    (void)sock;

    /* Example:
     * if(memcmp(line , "status_cmd" , strlen("status_cmd")) == 0){
     *     send_data_to_clients(SEND_TO_ALL, "status_res\n", strlen("status_res\n"));
     *     return true;
     * }
     */

    return false;
}

/* Which of the application's TCP commands may run WITHOUT pairing (see
 * doc/local-auth.md). Only consulted when LOCAL_AUTH_ENFORCE is 1. Return
 * true for harmless read-outs any phone on the network may issue; answer
 * those with send_raw_to_client(sock, ...) or SEND_TO_ALL_PUBLIC, since a
 * plain SEND_TO_ALL only reaches paired phones. Everything not listed here
 * needs a paired phone. */
bool app_command_is_public(const char *line , int len)
{
    (void)line;
    (void)len;

    /* Example:
     * if(len == 10 && memcmp(line , "status_cmd" , 10) == 0){
     *     return true;
     * }
     */

    return false;
}

/* Application MQTT commands. Called for every payload published on the
 * device's command topic (MQTT_COMMAND_TOPIC).
 *
 *   command         the payload — NOT NUL-terminated
 *   command_length  its length in bytes
 *
 * Reply with mqtt_publish_response("...") to publish "<id>:..." on the
 * response topic immediately, or send_data_to_clients(SEND_TO_ALL, ...) to
 * broadcast to every TCP client and MQTT (batched, see doc/send-buffers.md).
 * Return true once the command is handled; on false the default
 * "Hi I'm ESP32 Smart Device: Remote" reply is sent. Add new commands below. */
bool app_mqtt_command_handle(const char *command , int command_length)
{
    /* Example:
     * if(command_length >= 10 && memcmp(command , "status_cmd" , 10) == 0){
     *     mqtt_publish_response("status_res\n");
     *     return true;
     * }
     */

    return false;
}

void app_main(void)
{
    cything_begin();
}
