# CyThing

ESP32 smart-device firmware, packaged as a library. It brings up Wi-Fi
(pairing over BLE or a soft-AP → station), a TCP command server and UDP
discovery on the LAN, AWS IoT MQTT with per-device certificate provisioning, and HTTPS
OTA — and leaves exactly two functions for the device developer to write:

```cpp
#include <CyThingEsp32.h>

bool app_command_handle_line(const char *line, int len, int sock) {   // TCP
  if (String(line, len) == "relay_on") {
    digitalWrite(26, HIGH);
    send_data_to_clients(SEND_TO_ALL, "relay_res:1\n", 12);
    return true;
  }
  return false;
}

bool app_mqtt_command_handle(const char *command, int command_length) { // MQTT
  if (String(command, command_length) == "temp?") {
    mqtt_publish_response(("temp:" + String(analogRead(34)) + "\n").c_str());
    return true;
  }
  return false;
}

void setup() { cything_begin(); }
void loop()  { delay(1000); }
```

The full example is [examples/Basic/Basic.ino](examples/Basic/Basic.ino).
Design, rules for hook code, and the plan are in
[doc/cy_thing_lib.md](doc/cy_thing_lib.md).

## Using it

| | Arduino IDE | PlatformIO | ESP-IDF (`idf.py`) |
|---|---|---|---|
| Get the library | *Sketch → Include Library → Add .ZIP* (or Library Manager once published); also install **NimBLE-Arduino** (≥ 2.5.1) from Library Manager | `lib_deps = https://github.com/mbahmani90/cylinko_firmware.git#1.0.0` (NimBLE-Arduino is pulled in as a dependency) | clone this repo — it is the IDF project |
| Board package | *esp32 by Espressif Systems* 3.3.x | pioarduino platform (see [platformio.ini](platformio.ini)); the official `espressif32` platform is too old | ESP-IDF 6.0.x or 5.5.x |
| Partition table | copy `partitions/cything-<size>.csv` into the sketch folder as `partitions.csv`; set *Tools → Flash Size* to match | `board_build.partitions` + `board_upload.flash_size` (envs in `platformio.ini`) | `partitions.csv` (4 MB) or `sdkconfig.defaults.<size>` |
| Your commands | the two hooks in the sketch | same | `main/app_main.c` |
| Claim cert/key (optional) | paste the PEMs into the `claim_cert.pem.h` / `claim_key.pem.h` tabs — see below | same three files under `src/` | `main/claim_credentials.cpp` + the two `.pem.h` (see `.example`), then `idf.py reconfigure` |
| Channel/model config (optional) | the `cything_config.ino` + `cything_network_spec.h` + `cything_device_params.h` tabs, all three already in the sketch the app exports — see below | same three files under `src/` | `main/cything_config.cpp` + the two headers (see `.example`), then `idf.py reconfigure` |

### Channel and model configuration (optional)

Ports, the discovery multicast group, the soft-AP SSID prefix, the BLE base
UUID, the device name and type: all of them used to be `#define`s here, which
meant one firmware build per channel and a hand edit for every value. They are
now **weak symbols** (`src/device_config/cy_config.h`), and the application
overrides them the same way it overrides the claim credentials.

You do not normally write those files. In the phone app, open a device model and
tap **Download firmware sketch**: the ZIP is a complete sketch folder with
`cything_config.ino` and the two generated headers already in it, carrying that
channel's spec and that model's identity. Unzip, open, add your commands to the
hooks, Verify.

The values matter because a channel created in the app now draws its own BLE
service UUID and its own port — that is how the app tells your devices apart
from every other channel's, and a device built with different values is
invisible to it.

Nothing dropped in? The weak defaults in `src/device_config/cy_config.c` are the
old macro values, so the firmware behaves exactly as it always did. Check which
won with `nm`: `V`/`W` is the library default, `D`/`R` your override. The boot
log prints the effective spec on every start (`cy_config_log()`).

For ESP-IDF, copy `main/cything_config.cpp.example` to `main/cything_config.cpp`,
put the two generated headers next to it and run `idf.py reconfigure` once.

### Claim certificate (optional)

Certificates are not something most users touch: the Amazon root CA is
inside the library, and the device's own MQTT certificate is created by the
provisioning handshake and stored in NVS. The one exception is the
**per-model claim certificate + key**, which proves a unit is a genuine
device of your model during provisioning.

The example sketch has two tabs for them, `claim_cert.pem.h` and
`claim_key.pem.h`. Download the PEMs from the app's *Claim certificate*
screen and paste each one **verbatim** between the two marker lines — no
quoting, no `\n`:

```cpp
// claim_cert.pem.h — paste the claim certificate PEM between the marker lines
R"PEM(
-----BEGIN CERTIFICATE-----
MIIDWTCCAkGgAwIBAgIUQ...
...
-----END CERTIFICATE-----
)PEM"
```

```cpp
// claim_key.pem.h — same for the private key
R"PEM(
-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEAv...
...
-----END RSA PRIVATE KEY-----
)PEM"
```

That's all: the third tab, `claim_credentials.ino`, already turns those two
files into the strings the library links against, and the device starts
advertising `claim` in its `GET_INFO` reply. Leave them empty and the claim
step is simply skipped. Keep the `.pem.h` files out of git.

(Why `.pem.h` and not `.pem`: the Arduino IDE only carries `.h`/`.c`/`.cpp`/
`.ino` files into the build, so the file is a C++ raw string with a `.h`
name. PlatformIO: same three files under `src/`. ESP-IDF: copy
`main/claim_credentials.cpp.example` to `main/claim_credentials.cpp`, put the
two `.pem.h` files next to it, `idf.py reconfigure`.)

Default partition table is 4 MB, which runs on every ESP32; 8 MB and 16 MB
variants are in [partitions/](partitions/). Tested on ESP32; compiles for
ESP32-S3 and ESP32-C3 (not yet hardware-tested there).

Rules for hook code: they run on library tasks, not `loop()` — keep them
short, no `delay()`; `mqtt_publish_response()` only inside the MQTT hook;
don't use `WiFi.h`, the library owns the Wi-Fi driver.

## Building the IDF project

```bash
source ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

See [FLASHING.md](FLASHING.md) for erase-flash, partition-table changes and
the 8/16 MB variants.

## Repository layout

| Path | What |
|---|---|
| `src/` | the library's source — `CyThingEsp32.h`, `cything.c`, the protocol modules, vendored coreMQTT/backoffAlgorithm. Arduino/PlatformIO compile it as `srcDir` |
| `components/cything/` | the library as an ESP-IDF component (compiles `../../src`, `Kconfig.projbuild`) — what an external `idf.py` project adds to consume CyThing, see [cy_thing_lib.md](doc/cy_thing_lib.md) §5 |
| `main/` | ESP-IDF harness only: `app_main.c` (the hooks), `CMakeLists.txt` |
| `examples/Basic/` | reference sketch, `claim_credentials.ino` + the two `.pem.h` templates, `partitions.csv` |
| `partitions/` | partition tables per flash size |
| `doc/` | [cy_thing_lib.md](doc/cy_thing_lib.md) (the library), [tcp-server.md](doc/tcp-server.md), [local-auth.md](doc/local-auth.md) (password pairing + encrypted local link), [udp-discovery.md](doc/udp-discovery.md), [pairing.md](doc/pairing.md), [ble-pairing.md](doc/ble-pairing.md), [ble-scan-beacon.md](doc/ble-scan-beacon.md), [send-buffers.md](doc/send-buffers.md), [tasks.md](doc/tasks.md), [git-workflow.md](doc/git-workflow.md) |
| `scripts/release.sh` | cuts a release: one version into `library.properties`, `library.json`, `FIRMWARE_VERSION`; tag; push |
| `scripts/local_auth_client.py` | pair / authenticate / manage a device over the local link from a computer — see [doc/local-auth.md](doc/local-auth.md) |
| `scripts/ble_pair.py` | pair a device with a router over BLE from a computer (`pip install bleak`) — see [doc/ble-pairing.md](doc/ble-pairing.md) |

## Contributing / releasing

Every change goes through a branch and a PR ([doc/git-workflow.md](doc/git-workflow.md)).
Whenever `master` is stable, tag it: `scripts/release.sh MAJOR.MINOR.PATCH`.
