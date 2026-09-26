#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "CyThingEsp32.h"

#define LED_PIN GPIO_NUM_2

static const char *TAG = "on_off_pin";

/* Shared by both hooks: drive the pin for "on_cmd" / "off_cmd" and broadcast
 * the new state. SEND_TO_ALL reaches every TCP client and MQTT, so the phone
 * app's switch updates whichever way the command arrived. The reply must end
 * with '\n'. */
static bool handle_on_off(const char *cmd, int len)
{
    /* MQTT hands over the raw payload, which may still end in "\r\n". */
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r')) {
        len--;
    }

    if (len == 6 && memcmp(cmd, "on_cmd", 6) == 0) {
        gpio_set_level(LED_PIN, 1);
        send_data_to_clients(SEND_TO_ALL, "on_res\n", 7);
        return true;
    }
    if (len == 7 && memcmp(cmd, "off_cmd", 7) == 0) {
        gpio_set_level(LED_PIN, 0);
        send_data_to_clients(SEND_TO_ALL, "off_res\n", 8);
        return true;
    }
    return false;
}

/* A line over TCP that no built-in handler (provisioning, OTA, pairing)
 * claimed. `len` excludes the trailing newline. Return true once handled;
 * false lets the library log it as unrecognised. */
bool app_command_handle_line(const char *line, int len, int sock)
{
    (void)sock;
    return handle_on_off(line, len);
}

/* Which TCP commands work WITHOUT a paired phone once LOCAL_AUTH_ENFORCE is
 * on (doc/local-auth.md). Anything not listed needs a paired phone and
 * arrives encrypted. Switching the pin is not harmless, so nothing is public. */
bool app_command_is_public(const char *line, int len)
{
    (void)line;
    (void)len;
    return false;
}

/* A payload on the device's MQTT command topic. It is NOT NUL-terminated,
 * so always go through command_length. */
bool app_mqtt_command_handle(const char *command, int command_length)
{
    ESP_LOGI(TAG, "MQTT command: %.*s", command_length, command);
    return handle_on_off(command, command_length);
}

void app_main(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    cything_begin();
}
