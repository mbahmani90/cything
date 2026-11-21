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
static void set_pin(uint8_t on)
{
    gpio_set_level(LED_PIN, on);
    send_data_to_clients(SEND_TO_ALL, on ? "on_res\n" : "off_res\n", on ? 7 : 8);
    /* BLE phones subscribed to tx; an error (ignored) when there are none. */
    cything_ble_notify("tx", &on, 1);
}

static bool handle_on_off(const char *cmd, int len)
{
    /* MQTT hands over the raw payload, which may still end in "\r\n". */
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r')) {
        len--;
    }

    if (len == 6 && memcmp(cmd, "on_cmd", 6) == 0) {
        set_pin(1);
        return true;
    }
    if (len == 7 && memcmp(cmd, "off_cmd", 7) == 0) {
        set_pin(0);
        return true;
    }
    return false;
}

/* ---- BLE: station-mode GATT server (doc/ble-gatt-server.md) ----------
 *
 * The channel's own BLE table. The phone app generates this into
 * cything_network_spec.h (static CYTHING_BLE_SERVICES + a
 * CYTHING_BLE_SERVICE_COUNT) and its cything_config.cpp hands it over as
 * the two cy_ble_* symbols; this example writes them by hand. UUIDs are the
 * default base 6A48xxxx-DC02-4C78-A1A3-64193A12E418, little-endian, slot at
 * bytes 12/13:
 *   1101 service · 1102 rx (phone writes) · 1103 tx (device notifies) */
#define BASE_UUID_LE(lo, hi) \
    { 0x18, 0xE4, 0x12, 0x3A, 0x19, 0x64, 0xA3, 0xA1, 0x78, 0x4C, 0x02, 0xDC, (lo), (hi), 0x48, 0x6A }

static const cything_ble_chr_t data_chrs[] = {
    { .name = "rx", .uuid128 = BASE_UUID_LE(0x02, 0x11), .flags = CYTHING_BLE_WRITE },
    { .name = "tx", .uuid128 = BASE_UUID_LE(0x03, 0x11), .flags = CYTHING_BLE_NOTIFY },
};

static const cything_ble_svc_t ble_services[] = {
    { .uuid128 = BASE_UUID_LE(0x01, 0x11), .chrs = data_chrs, .chr_count = 2 },
};

const cything_ble_svc_t *const cy_ble_services = ble_services;
const uint8_t cy_ble_service_count = sizeof(ble_services) / sizeof(ble_services[0]);

/* One byte on rx: 0x01 on, 0x00 off. Runs on the NimBLE host task. */
bool app_ble_write(const char *name, const uint8_t *data, int len)
{
    if (strcmp(name, "rx") == 0 && len == 1 && data[0] <= 1) {
        set_pin(data[0]);
        return true;
    }
    return false;    /* the phone sees ATT error CYTHING_BLE_ATT_ERR_REJECTED */
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
