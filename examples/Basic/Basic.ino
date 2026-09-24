/*
 * CyThing — minimal sketch.
 *
 * The library brings up everything (Wi-Fi pairing / station, TCP + UDP local
 * servers, AWS IoT MQTT, OTA) from cything_begin(). The sketch only adds the
 * device's own commands by defining the two hooks below. See
 * doc/cy_thing_lib.md in the library for the rules (short, non-blocking, no
 * WiFi.h) and the reply helpers.
 *
 * This sketch needs the partitions.csv next to it (the Arduino IDE picks it
 * up automatically; PlatformIO via board_build.partitions).
 */
#include <CyThingEsp32.h>

/* Pins are the sketch's business, not the library's — pick ones that exist
 * on your module. These defaults suit the classic ESP32 DevKit; on an
 * ESP32-S3 use e.g. RELAY_PIN 4 and SENSOR_PIN 1 (S3 ADC1 is GPIO1-10). */
#define RELAY_PIN   26
#define SENSOR_PIN  34   // ADC input

/* A line received over TCP that none of the built-in handlers (provisioning,
 * OTA, pairing) claimed. `len` excludes the trailing '\n'. */
bool app_command_handle_line(const char *line, int len, int sock) {
  String cmd(line, len);

  if (cmd == "relay_on") {
    digitalWrite(RELAY_PIN, HIGH);
    send_data_to_clients(SEND_TO_ALL, "relay_res:1\n", 12);   // every TCP client + MQTT
    return true;
  }
  if (cmd == "relay_off") {
    digitalWrite(RELAY_PIN, LOW);
    send_data_to_clients(SEND_TO_ALL, "relay_res:0\n", 12);
    return true;
  }
  /* on_cmd / off_cmd are what the phone app's on/off switch sends. The library
   * used to answer them itself; it no longer does, because what a command
   * actually switches is the device's business. Answer with on_res/off_res so
   * the app's switch tracks the real state. */
  if (cmd == "on_cmd") {
    digitalWrite(RELAY_PIN, HIGH);
    send_data_to_clients(SEND_TO_ALL, "on_res\n", 7);
    return true;
  }
  if (cmd == "off_cmd") {
    digitalWrite(RELAY_PIN, LOW);
    send_data_to_clients(SEND_TO_ALL, "off_res\n", 8);
    return true;
  }
  if (cmd == "whoami") {
    send_raw_to_client(sock, "cything-basic");               // only the sender
    return true;
  }
  return false;                                              // logged as unrecognised
}

/* Which TCP commands work WITHOUT pairing once LOCAL_AUTH_ENFORCE is on
 * (doc/local-auth.md). "whoami" is harmless, so any phone on the network
 * may ask; relay_on/relay_off are not listed, so they need a paired phone
 * and arrive encrypted. */
bool app_command_is_public(const char *line, int len) {
  String cmd(line, len);
  return cmd == "whoami";
}

/* A payload received on the device's MQTT command topic. The payload is NOT
 * NUL-terminated — always go through command_length. */
bool app_mqtt_command_handle(const char *command, int command_length) {
  String cmd(command, command_length);

  if (cmd == "temp?") {
    String res = "temp:" + String(analogRead(SENSOR_PIN)) + "\n";
    mqtt_publish_response(res.c_str());                      // "<id>:temp:NNN" on the response topic
    return true;
  }
  return false;                                              // default "Hi I'm ESP32 ..." reply
}

/* Optional: per-model claim certificate + key for the provisioning claim
 * step — paste them into the claim_cert.pem.h / claim_key.pem.h tabs (see
 * claim_credentials.ino). Left empty, the step is skipped. */

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(SENSOR_PIN, INPUT);
  cything_begin();
}

void loop() {
  delay(1000);   // everything runs in the library's FreeRTOS tasks
}
