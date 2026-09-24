# CyThing — packaging the firmware as a library

**Goal:** a developer adds a device-specific command by writing two functions
in one file — nothing else in the firmware needs to be opened:

```c
bool app_command_handle_line(const char *line , int len , int sock);   /* TCP */
bool app_mqtt_command_handle(const char *command , int command_length); /* MQTT */
```

They are declared in [src/CyThingEsp32.h](../src/CyThingEsp32.h) and, under ESP-IDF,
defined in [main/app_main.c](../main/app_main.c). This document
records how they work, and the plan for shipping the rest of the firmware as a
library called **CyThing** that Arduino IDE users, PlatformIO users and the
existing `idf.py` workflow can all consume from the same source tree.

Status: **packaged.** Hooks, IDF 5.5/6.0 compatibility, the
`cything_begin()` / `CyThingEsp32.h` split, the `src/` re-layout and the
Arduino/PlatformIO packaging are done (§8). What remains is hardware
verification of the Arduino-built firmware.

## 1. The two hooks (already in place)

Each hook is a *weak* symbol with a no-op default inside the module that owns
the protocol, and a strong definition in the application (`app_main.c` under
ESP-IDF). The linker picks the strong one; if the application stops defining
it, the firmware still links and the default behaviour returns.

Boot logic lives in [src/cything.c](../src/cything.c) as `cything_begin()`;
[main/app_main.c](../main/app_main.c) is now only the two hooks plus
`app_main(){ cything_begin(); }` — i.e. exactly what a sketch will contain.
[src/CyThingEsp32.h](../src/CyThingEsp32.h) declares `cything_begin()`, both hooks and
the reply helpers inside `extern "C"` (the `tcp_client_list.h` include sits
*inside* that block on purpose — outside it, a C++ sketch would look for a
mangled `send_data_to_clients`; verified with a host `clang++` + `nm`).

| Hook | Called from | Default when it returns `false` |
|---|---|---|
| `app_command_handle_line(line, len, sock)` | [`tcp_dispatch_line`](../src/tcp_server/tcp_command.c), after provisioning → OTA → pairing → built-in `on_cmd`/`off_cmd` | line is logged as `unrecognised line ignored` |
| `app_mqtt_command_handle(command, command_length)` | [`process_aws_iot_command`](../src/aws/aws_iot_handler.c), for every payload on `MQTT_COMMAND_TOPIC` | publishes `"<id>:Hi I'm ESP32 Smart Device: Remote\n"` |

Reply helpers available inside the hooks:

| Helper | Where it goes | Timing |
|---|---|---|
| `send_data_to_clients(SEND_TO_ALL, data, len)` | every TCP client **and** MQTT (mirrored into `mqtt_response_fifo`) | TCP immediately, MQTT batched every `mqtt_response_get_period_ms()` — see [send-buffers.md](send-buffers.md) |
| `send_raw_to_client(sock, text)` | the one TCP client that sent the line, no response id | immediately |
| `mqtt_publish_response(text)` | MQTT response topic as `"<id>:text"` | immediately; **only valid inside `app_mqtt_command_handle`** (needs the live MQTT context, which belongs to `aws_iot_task`) |

Argument conventions:

- TCP `line` is NUL-terminated with the trailing `'\n'` still attached; `len`
  excludes the `'\n'`.
- MQTT `command` is **not** NUL-terminated — always use `command_length`
  (`memcmp`, `%.*s`, or `String(command, command_length)` in Arduino).

Rules for hook code, because the hooks are *not* called from `loop()`:

1. They run on library tasks (a TCP client task, `aws_iot_task`). Keep them
   short and non-blocking — no `delay()`, no long I/O. A `delay(5000)` in the
   MQTT hook stalls the MQTT process loop and keep-alive. For long work set a
   flag or queue and do it in `loop()`.
2. `mqtt_publish_response()` only inside the MQTT hook;
   `send_data_to_clients()` works from anywhere.
3. Do not use Arduino's `WiFi.h` — the library owns the Wi-Fi driver (STA/AP,
   pairing, reconnect). `pinMode`/`digitalWrite` on your own pins are fine.

The old `gpio/` module (four hard-coded output pins) was removed: with the
hooks, pin setup belongs to the application (`pinMode()` in `setup()` or
`gpio_config()` in `app_main`).

## 2. Which IDE — what each one can and cannot do

| | ESP-IDF (`idf.py`) — today | Arduino IDE | PlatformIO |
|---|---|---|---|
| Editor | any + terminal | Arduino IDE | VS Code (extension) or `pio` CLI |
| Code style | `app_main()`, IDF APIs | `setup()`/`loop()` | either, or both together (`framework = arduino, espidf`) |
| Installs ESP-IDF itself? | yes (manual: `export.sh`, `IDF_PATH`) | no — board package ships a **precompiled** IDF | no — downloaded automatically per `platformio.ini` |
| `sdkconfig` / menuconfig | full control | **none** (fixed at core build time) | full control only with `framework = espidf` |
| Custom IDF components (coreMQTT, `esp_secure_cert_mgr`) | yes | no — must be vendored into the library `src/` | yes with `espidf`; no with plain `arduino` |
| Custom `partitions.csv` | yes | yes (`partitions.csv` in the sketch folder) | yes (`board_build.partitions`) |
| Reproducible toolchain | manual | board package version | pinned in `platformio.ini` |

Key facts that drive the design:

- **Arduino IDE is the stricter target.** A library that works there also works
  in PlatformIO with `framework = arduino` (PlatformIO reads the same
  `library.properties`). The reverse is not true: a PlatformIO-hybrid project
  (`arduino, espidf`) cannot be opened in the Arduino IDE.
- **PlatformIO users do not work in the Arduino IDE.** They use VS Code with the
  PlatformIO extension. The sketch code is identical.
- **The weak-symbol override is exactly how Arduino already works**
  (`setup`/`loop`/`serialEvent`), so the hooks need no change in mechanism —
  only C linkage (see §4).

## 3. ESP-IDF version — the one real constraint

| | IDF |
|---|---|
| This project (`sdkconfig`, [git-workflow.md](git-workflow.md)) | **6.0.2** |
| arduino-esp32 3.3.11 (Jul 2026) — the Arduino IDE board package and pioarduino `55.03.311` | **5.5.5** |
| arduino-esp32 4.0 (IDF 6 based) | `4.0.0-alpha1` only (May 2026); no stable, no pioarduino release |
| Official PlatformIO `espressif32` platform | stuck at arduino-esp32 2.x / IDF 4.4 — not an option; use the community `pioarduino` platform |

So **any Arduino-flavoured route means building this code on IDF 5.5**, and the
project was deliberately moved to IDF 6 / mbedTLS 4 (commit `61a0b7d`).

**Trial build on IDF 5.5.5 (done, 2026-09-13):** with a clean checkout, every
source compiled except [src/aws/provisioning.c](../src/aws/provisioning.c)
— six call sites, four mbedTLS 3.x/4.x API differences:

| API | mbedTLS 3.x (IDF 5.5) | mbedTLS 4.x (IDF 6.0) |
|---|---|---|
| `mbedtls_pk_parse_key()` | takes `f_rng, p_rng` | no RNG args |
| `mbedtls_pk_sign()` | takes `f_rng, p_rng` | no RNG args |
| `mbedtls_x509write_csr_der()` | takes `f_rng, p_rng` | no RNG args |
| EC key generation | `mbedtls_pk_setup()` + `mbedtls_ecp_gen_key()` | `psa_generate_key()` + `mbedtls_pk_wrap_psa()` (transparent PK contexts removed) |

`provisioning.c` now hides these behind `#if MBEDTLS_VERSION_MAJOR >= 4`:
`PROV_PK_PARSE_KEY` / `PROV_PK_SIGN` / `PROV_CSR_WRITE_DER` macros and a
`prov_generate_key_pem()` with one body per major version. The 3.x branch
feeds `esp_fill_random()` as the RNG and does not use PSA at all, so it does
not depend on whether a precompiled Arduino core enables `MBEDTLS_PSA_CRYPTO_C`.
The stored-key format (PEM in NVS) and the CSR are identical on both.

Everything else is already compatible. (`esp_secure_cert_mgr`, which had an
IDF-6 patch, turned out to be unused and was dropped in the re-layout — §4.)

Result: **the same tree builds and links on IDF 5.5.5 and 6.0.2.** Still
outstanding: a hardware test of the provisioning handshake (CSR → cert) on a
5.5 build. IDF 5.5.5 is installed next to 6.0.2 at `~/.espressif/v5.5.5`
(`. ~/.espressif/v5.5.5/esp-idf/export.sh`); build in a separate copy or with
`-B build55 -DSDKCONFIG=sdkconfig55` so the two `sdkconfig`s do not fight.

Options that were considered: support both (chosen — the divergence turned
out to be one file), wait for arduino-esp32 4.0 stable (no date), or move the
project back to 5.5 (undoes the IDF-6 work).

## 4. Layout — one tree, three consumers

This is the tree as it is now (re-layout done):

```
smart_device_test_gpio/
├── src/                           ← the library's source. Arduino/PlatformIO compile all of it directly (srcDir);
│   │                                 the IDF build compiles it via the `cything` component below
│   ├── CyThingEsp32.h                  extern "C" umbrella: cything_begin() + the 2 hooks + reply helpers
│   ├── cything.c                  boot logic, cything_begin()
│   ├── tcp_server/ aws/ wifi/ udp_socket/ ota_lib/ memory_handler/ common/ device_config/
│   ├── ble/                       BLE (NimBLE; NimBLE-Arduino under Arduino): Wi-Fi pairing service — doc/ble-pairing.md;
│   │                              station-mode scan beacon — doc/ble-scan-beacon.md
│   ├── coreMQTT/                  vendored, flattened: core_mqtt*.c/h, transport_interface.h,
│   │                                 core_mqtt_config.h, network_transport.c/h (ESP-TLS port),
│   │                                 logging_levels.h / logging_stack.h, LICENSE
│   ├── backoffAlgorithm/          vendored: backoff_algorithm.c/h, LICENSE
│   └── posix_compat/              clock.h / clock_esp.c (coreMQTT's time source)
├── components/cything/            ← the library as an ESP-IDF component — what an external
│   │                                 idf.py project adds to consume CyThing (§5)
│   ├── CMakeLists.txt             idf_component_register: lists ../../src/**.c, INCLUDE_DIRS ../../src, WHOLE_ARCHIVE
│   └── Kconfig.projbuild          MQTT endpoint/client-id options + the coreMQTT menu
├── main/                          ← ESP-IDF harness only, for this repo's own firmware
│   ├── app_main.c                 the two hooks + app_main(){ cything_begin(); } — exactly what a sketch contains
│   └── CMakeLists.txt             app_main.c (+ optional claim/config overrides), PRIV_REQUIRES cything
├── CMakeLists.txt                 root IDF project (no EXTRA_COMPONENT_DIRS needed — components/ is auto-discovered)
├── partitions/                        cything-4MB.csv (default) / -8MB / -16MB — see "Partition tables"
├── partitions.csv                     = cything-4MB.csv, what sdkconfig points at
├── sdkconfig, sdkconfig.defaults, sdkconfig.defaults.{8mb,16mb}
├── library.properties, library.json   Arduino IDE / PlatformIO metadata (libArchive=false, see §4)
├── examples/Basic/                    Basic.ino + partitions.csv (= the 4 MB table; the sketch folder needs both)
└── platformio.ini                     builds examples/Basic against the repo itself (pioarduino), one env per flash size
```

Only `src/` is on the include path, in every build (via the `cything`
component's `INCLUDE_DIRS` under ESP-IDF, via `srcDir` under Arduino/
PlatformIO). Includes of the vendored libraries are therefore prefixed
(`coreMQTT/core_mqtt.h`, `backoffAlgorithm/backoff_algorithm.h`,
`posix_compat/clock.h`); the vendored files' own includes are same-directory
and unchanged.

`components/cything/` holds no source of its own — it is a thin
`idf_component_register` wrapper naming files under `../../src`, so `src/`
itself never moves and the Arduino/PlatformIO packaging (`library.json`
`srcDir: src`) is unaffected by it existing. This repo's own build picks it
up automatically: ESP-IDF scans `PROJECT_DIR/components/*` with no
`EXTRA_COMPONENT_DIRS` entry needed. An external `idf.py` project instead
points `EXTRA_COMPONENT_DIRS` at a checkout of this repo's `components/`
directory (§5).

### Removed in the re-layout (all unreachable in this configuration)

| Removed | Why |
|---|---|
| `managed_components/espressif__esp_secure_cert_mgr` + `main/idf_component.yml` + `dependencies.lock` | only referenced under `CONFIG_EXAMPLE_USE_ESP_SECURE_CERT_MGR`, which was never set — the cert comes from provisioning (NVS, `EXAMPLE_USE_PLAIN_FLASH_STORAGE`). The IDF-6 patch it carried (commit `61a0b7d`) is gone with it |
| the `EXAMPLE_CHOOSE_PKI_ACCESS_METHOD` Kconfig choice and the `SECURE_ELEMENT` / `ESP_SECURE_CERT_MGR` branches in `aws_iot_handler.c` | those two options pointed at components that were never built; only the plain-flash path remains, now unconditional |
| `$IDF_PATH/examples/common_components/protocol_examples_common` (+ its `ethernet_init` dependency) | nothing included it; it only added `CONFIG_EXAMPLE_CONNECT_*` options to `sdkconfig` |
| `libraries/coreJSON`, `libraries/corePKCS11` | never in `EXTRA_COMPONENT_DIRS` |
| `libraries/coreMQTT`, `libraries/backoffAlgorithm`, `libraries/common` | moved into `src/` (flattened; docs, CI files and `semaphore.[ch]` dropped) |

`sdkconfig` lost the `CONFIG_EXAMPLE_*`, `CONFIG_ENV_GPIO_*`, `CONFIG_ETH*`
and `CONFIG_ESP_SECURE_CERT_*` entries; the coreMQTT `CONFIG_MQTT_*` /
`CONFIG_CORE_MQTT_LOG_*` values are unchanged (their Kconfig moved into
`main/Kconfig.projbuild`). Verified: `idf.py fullclean && idf.py build` on
IDF 6.0.2 and 5.5.5.

### What the Arduino build needed (step 6, done)

- **Kconfig fallbacks.** `coreMQTT/core_mqtt_config.h` and `aws/demo_config.h`
  read `CONFIG_MQTT_*` / `CONFIG_CORE_MQTT_LOG_*` from `sdkconfig.h`; the
  precompiled Arduino core's `sdkconfig.h` has none of them. Each now has an
  `#ifndef` default equal to the Kconfig default. The four `CORE_MQTT_LOG_*`
  bools get an error+info default only under `#ifdef ARDUINO` (a Kconfig
  bool set to `n` is simply absent, so "all absent" cannot be told from "all
  off" in an IDF build). The unused `CONFIG_MQTT_BROKER_ENDPOINT` /
  `CONFIG_MQTT_CLIENT_IDENTIFIER` options (endpoint and client id come from
  provisioning) were removed along with the `#error` that demanded them.
- **PEMs as C strings.** `target_add_binary_data` has no Arduino equivalent.
  Amazon Root CA 1 is public and now lives in
  `src/device_config/root_cert_auth.c` (`root_cert_auth_pem`,
  `root_cert_auth_pem_len` — `sizeof`, i.e. NUL included, as mbedTLS wants).
  The per-model **claim cert + key** are secret, so the library ships empty
  `__attribute__((weak))` defaults in `src/device_config/claim_credentials.c`
  and the *application* overrides them with strong definitions — the same
  mechanism as the hooks. The PEMs are pasted verbatim into two C++
  raw-string files, `claim_cert.pem.h` / `claim_key.pem.h` (the Arduino IDE
  cannot embed a plain `.pem`; it only carries `.h`/`.c`/`.cpp`/`.ino` into
  the build), which a `claim_credentials.ino` / `.cpp` `#include`s into the
  two arrays. Because an empty template still holds a newline, "present" is
  `claim_credentials_present()` = both contain `-----BEGIN`, not
  `[0] != '\0'`. Under ESP-IDF: git-ignored `main/claim_credentials.cpp`
  (template `.cpp.example`) + the two `.pem.h`, `idf.py reconfigure` once.
  Verified with `nm`: `V` (weak) without, `D` with.
- **Channel spec as symbols, not macros.** `TCP_PORT`, `UDP_PORT`,
  `SCAN_COMMAND`, `MULTICAST_IPV4_ADDR`, `ESP_AP_WIFI_SSID_PREFIX`,
  `DEVICE_NAME`/`DEVICE_TYPE` and the BLE base UUID moved from `#define` to
  weak `const` symbols in `src/device_config/cy_config.c`, because the phone app
  exports them per channel and per model (a sketch bundle with
  `cything_network_spec.h` + `cything_device_params.h`). The defaults *are* the
  old macros, so a build with no override is unchanged. Two consequences worth
  knowing: `SCAN_COMMAND_LEN` was a `sizeof`, so it became
  `cy_scan_command_len()` — still `strlen - 2`, bug-for-bug, because the loose
  match is what older firmware and the app's probes expect; and the BLE UUIDs
  could no longer be `BLE_UUID128_INIT` static initializers, so `ble_pairing.c`
  and `ble_beacon.c` now fill non-`const` `ble_uuid128_t`s via
  `ble_uuid128_from_base()` before the GATT table is registered or anything is
  advertised. The `gatt_svcs` table is untouched: it stores *addresses*, which
  are as constant as ever. The same C++ internal-linkage trap as the PEMs
  applies, which is why `CyThingEsp32.h` includes `cy_config.h` inside its
  `extern "C"` block and every override template includes `CyThingEsp32.h`.
- **Weak symbols vs static archives.** A linker only extracts an archive
  member when it resolves a currently-undefined symbol, and a weak definition
  counts — so if the library's weak default sits earlier in the archive than
  the application's strong one, the weak one silently wins. Three builds,
  three fixes: ESP-IDF — the `cything` component (which owns every weak
  default: the two hooks, `app_command_is_public`, `claim_cert_pem`/
  `claim_key_pem`, the `cy_config.c` channel-spec symbols) is registered with
  `WHOLE_ARCHIVE`, forcing all of it into the link so the application's
  (`main`'s) strong definitions always win regardless of extraction order —
  verified both ways with `nm` on the linked `.elf` (`T`/`D` for `main`'s
  overrides, `V` for anything `main` leaves unoverridden) on IDF 6.0.2 and
  5.5.5; PlatformIO — `library.json` sets `"libArchive": false` (PlatformIO's
  own recommendation for weak symbols); Arduino IDE — library objects are
  linked directly unless `dot_a_linkage=true`, which we do not set.
- **`sdkconfig` audit** against arduino-esp32 3.3's `defconfig` (lib-builder
  `configs/defconfig.common` + `defconfig.esp32`): nothing in `src/` depends
  on a non-default option. Differences worth knowing: `FREERTOS_HZ` is 1000
  (ours 100 — all delays go through `portTICK_PERIOD_MS`/`pdMS_TO_TICKS`, so
  no change); `LWIP_MAX_SOCKETS` 16 (ours 10); `BOOTLOADER_APP_ROLLBACK_ENABLE=y`
  — after an OTA the new image boots as *pending-verify* and the Arduino core
  marks it valid in `initArduino()` unless the sketch defines
  `verifyRollbackLater()`, so our `flash_boot_handler` flow is unaffected;
  `MBEDTLS_DYNAMIC_BUFFER=y` (less TLS RAM, no API change).
- **Partition table.** Arduino has no `PARTITION_TABLE_CSV`; the Arduino IDE
  uses a `partitions.csv` found in the sketch folder (verified: the built
  `partitions.bin` carries our entries), PlatformIO `board_build.partitions`.
  The old table (three 1 MiB app slots + `factory`, plus `sip`/`pat_*`/
  `accounts` that nothing referenced) had two problems under the Arduino
  core: the Arduino-built image is ~1.03 MB, leaving 3–14 KB in a 1 MiB
  slot, and Arduino tooling assumes the Arduino layout (`otadata` at 0xe000,
  app flashed to `ota_0` at 0x10000, no `factory`) — pioarduino was writing
  `boot_app0.bin` into the middle of our `otadata` and the app into a
  different slot than the Arduino IDE. See "Partition tables" below for the
  replacement.

### Partition tables (`partitions/`)

Three variants, one per flash size, same shape — see the CSVs for the exact
offsets:

| | `cything-4MB.csv` (default) | `cything-8MB.csv` | `cything-16MB.csv` |
|---|---|---|---|
| `nvs` / `otadata` | 0x9000 / 0xE000 (Arduino-standard) | same | same |
| `ota_0`, `ota_1` | 2 × 1.5 MB | 2 × 3 MB | 2 × 4 MB |
| `wifi_info`, `rst_count`, `boot` | 3 × 4 KB after `ota_1` | same | same |
| `storage` (spiffs, for the sketch) | 948 KB | ~1.9 MB | ~7.9 MB |

- **No `factory` slot.** A/B OTA only needs two: first flash lands in
  `ota_0`, each OTA writes the other slot and flips `otadata`; the previous
  image is always still in the other slot for rollback. `flash_boot_handler`
  now accepts only `OTA_0`/`OTA_1` as boot targets and clears anything else
  with a warning; the never-called `ota_read_factory_partition()` debug
  helper (which would have crashed on a missing factory partition) is gone.
- **Every partition is found by type/subtype**, never by offset, so the
  firmware image is identical for all variants — only the table differs.
- **The table is fixed at build time and OTA cannot change it.** A 4 MB table
  runs on any ESP32 (all have ≥ 4 MB), so it is the default everywhere; a
  table larger than the chip does not boot. `cything_begin()` logs a warning
  when the chip has more flash than the table covers, pointing at the
  variants. Moving a device between variants needs a full serial flash.
- **Selecting a variant:**

  | Tool | How |
  |---|---|
  | Arduino IDE | copy the CSV into the sketch folder as `partitions.csv` and set *Tools → Flash Size* to match. A library cannot add *Partition Scheme* menu entries; a CyThing board package (own `boards.txt` + variants referencing the esp32 core) would make "pick the board" sufficient — a follow-up |
  | PlatformIO | `pio run -e esp32-4mb` / `esp32-8mb` / `esp32-16mb` / `esp32s3-8mb` (`platformio.ini`); consumers set `board_build.partitions` + `board_upload.flash_size` |
  | ESP-IDF | default: `sdkconfig` → `partitions.csv` (= the 4 MB table). Variant: `idf.py -B build-8mb -DSDKCONFIG=build-8mb/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.8mb" build` (`sdkconfig.defaults` is the project's minimal config, `sdkconfig.defaults.<size>` adds flash size + table) |

- Verified: all three CSVs pass `gen_esp32part.py`; PlatformIO builds all
  three envs (4 MB: 66 % of the slot used; pioarduino now flashes to 0x10000
  with `boot_app0.bin` at 0xE000, i.e. our `otadata`); arduino-cli's
  `partitions.bin` is our table; IDF 6.0.2 default and `-8mb` variant, IDF
  5.5.5 default.
- Devices flashed with the old table need one full `idf.py flash` (the app
  moves from `factory` to `ota_0`); `wifi_info` and `rst_count` keep their
  offsets, so pairing data and the reset counter survive.

### Code changes — needed for every route

- ~~`app_main()` → `cything_begin()`~~ done. The arduino-esp32 core defines its
  own `app_main()` (it is what calls `setup()`/`loop()`), so ours would be a
  duplicate symbol. `main/app_main.c` keeps a one-line `app_main()` for `idf.py`.
- ~~`CyThingEsp32.h` with `extern "C" { … }` around the public API~~ done. `.ino`
  files are C++; without C linkage a sketch's `app_command_handle_line` would
  get a mangled name and **not** override the C symbol. Because the
  declaration is `extern "C"`, the sketch author writes the function normally.
- ~~`provisioning.c` mbedTLS 3.x path (§3).~~ done.

### Extra for Arduino IDE compatibility

- ~~Arduino puts only `<lib>/src` on the include path, so
  `#include "core_mqtt.h"` → `#include "coreMQTT/core_mqtt.h"`~~ done.
- ~~`esp_secure_cert_mgr` vendored into `src/`~~ dropped instead — it was
  unused (see the table above).
- ~~`sdkconfig` audit against the precompiled core's config~~ done, see
  "What the Arduino build needed" above.

Nothing in `tcp_server/`, `wifi/`, `udp_socket/`, `ota_lib/`,
`memory_handler/`, `common/` needed to change.

### What Arduino users give up

`sdkconfig` control. They get whatever the arduino-esp32 core was built with.
Task stack sizes and priorities are ours ([common/task_config.h](../src/common/task_config.h)),
so those are unaffected.

## 5. Using the library

The canonical sketch is [examples/Basic/Basic.ino](../examples/Basic/Basic.ino):
`#include <CyThingEsp32.h>`, the two hooks, `cything_begin()` from `setup()`,
an empty `loop()`. Any Arduino API or library can be used inside the hooks,
subject to the three rules in §1.

| | Arduino IDE | PlatformIO | ESP-IDF (external project) |
|---|---|---|---|
| Install | Sketch → Include Library → Add .ZIP (or Library Manager once published) | `lib_deps = https://github.com/mbahmani90/cything.git` (pin a tag: `…git#1.0.0`) | `idf.py add-dependency "mbahmani90/cything^1.0.0"` once published to the Component Registry (§5's "Registry publishing"); until then, clone/submodule the repo anywhere and, in the consumer's own root `CMakeLists.txt`, before `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`: `set(EXTRA_COMPONENT_DIRS "<path>/cything/components")` (or symlink `components/cything` into the consumer's own `components/`) |
| `sdkconfig` | n/a — Arduino core is precompiled | n/a with `framework = arduino` | **copy this repo's [sdkconfig.defaults](../sdkconfig.defaults) into the consumer's own project root.** It is not optional: `CONFIG_BT_ENABLED` / `CONFIG_BT_NIMBLE_ENABLED` (pairing needs NimBLE — without them IDF's `bt` component doesn't even expose `esp_bt.h`, a hard compile error in `src/ble/`), `CONFIG_PARTITION_TABLE_CUSTOM*` (points at `partitions.csv`), `CONFIG_MBEDTLS_X509_CREATE_C` (provisioning's CSR). Confirmed by building a real standalone project against `components/cything` |
| Board package | Boards Manager → *esp32 by Espressif Systems* 3.3.x | `platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip`, `framework = arduino` | whatever `idf.py set-target` the consumer already uses — nothing CyThing-specific |
| Partition table | copy `partitions/cything-<size>.csv` into the sketch folder as `partitions.csv` (4 MB unless you know your module is bigger) and set *Tools → Flash Size* to match | `board_build.partitions = cything-<size>.csv`, `board_upload.flash_size = <size>` | copy `partitions/cything-<size>.csv` in as the consumer's own `partitions.csv`, same as this repo's root `CMakeLists.txt` (`set(PARTITION_TABLE_CSV "partitions.csv")`) |
| Claim cert/key (optional) | paste the PEMs verbatim into the `claim_cert.pem.h` / `claim_key.pem.h` tabs (C++ raw strings; `claim_credentials.ino` `#include`s them into `claim_cert_pem` / `claim_key_pem`, which `CyThingEsp32.h` declares `extern "C"` — without that a C++ `const` would be file-local and silently ignored); keep the `.pem.h` files out of git | same three files under `src/` | same `claim_credentials.cpp` pattern as this repo's `main/` (see `main/claim_credentials.cpp.example`) |
| Update | re-add the ZIP / Library Manager "update" | `pio pkg update`, or bump the tag | `git submodule update` / re-clone, or bump the pinned tag |

The consumer's own `main/` is shaped exactly like this repo's: implement
`app_command_handle_line` / `app_mqtt_command_handle` (+ optionally
`app_command_is_public`), call `cything_begin()` from `app_main()`, and
`PRIV_REQUIRES cything` in its `idf_component_register()` call —
**`mbahmani90__cything` instead**, if resolved through the Component
Registry rather than `EXTRA_COMPONENT_DIRS`/a submodule (see below: the
registry namespaces the component under `managed_components/`, a plain
clone doesn't).

### Registry publishing

`components/cything/idf_component.yml` is the manifest; `components/cything/`
itself still points at `../../src` (unchanged, for the reasons in §4), but
the registry distributes each component as a self-contained archive — it
packs only what's under the directory it's given, nothing outside it.
`scripts/pack_component.sh` bridges the gap: it assembles a throwaway,
self-contained copy (`src/` copied in, the `CMakeLists.txt` path rewritten to
local) in a scratch directory, runs `compote component pack` against *that*,
and deletes the scratch copy — nothing about the committed layout changes.

```bash
scripts/pack_component.sh 1.0.0     # -> dist/pack/cything_1.0.0.tgz
```

Verified (2026-09-24): the packed archive resolves and builds through the
*real* component-manager dependency path — a consumer `main/idf_component.yml`
with a `path:` dependency pointing at the extracted archive, `idf.py build`,
full manifest parse + `dependencies.lock` + build, no shortcuts. One naming
detail this surfaced: once actually published and pulled via
`idf.py add-dependency mbahmani90/cything`, the component manager downloads
it into `managed_components/mbahmani90__cything/`, so a consumer's
`main/CMakeLists.txt` needs `PRIV_REQUIRES mbahmani90__cything` (the
namespaced form), not `PRIV_REQUIRES cything`.

To actually publish: `compote registry login` (needs an Espressif account —
not something a git clone or this script can do for you), then
`compote component upload --namespace mbahmani90 --name cything --archive
dist/pack/cything_1.0.0.tgz`. Not done yet.

Do not use `WiFi.h` in the sketch; the library owns the Wi-Fi driver.

## 6. Chips

Nothing in `src/` is chip-specific — no ROM/SoC includes, no hard-coded
GPIOs (the old `gpio/` module was the only one), everything through portable
IDF APIs — so the library builds for the whole esp32 family:

| Chip | Build | Hardware-tested | Notes |
|---|---|---|---|
| ESP32 | ✅ IDF 6.0.2 / 5.5.5, PlatformIO, arduino-cli | pending (Arduino build) | the target so far |
| ESP32-S3 | ✅ arduino-cli `esp32:esp32:esp32s3`, PlatformIO env `esp32s3-8mb` | **no** | modules are usually 8/16 MB → 8 MB table by default; hardware AES/SHA path in mbedTLS differs from ESP32, so the provisioning handshake needs a pass; check Wi-Fi on modules with PSRAM |
| ESP32-C3 (RISC-V) | ✅ arduino-cli `esp32:esp32:esp32c3` | no | 1.13 MB image (RISC-V code is larger) — still fits a 1.5 MB slot |

`library.properties` says `architectures=esp32`, which is Arduino's name for
the whole family. `CyThingEsp32.h` keeps its name — "ESP32" is the family
name in Arduino usage, and the header is identical for every member. For
`idf.py`, `idf.py set-target esp32s3` regenerates `sdkconfig` from
`sdkconfig.defaults`. The example sketch's pins (`RELAY_PIN`, `SENSOR_PIN`)
are the sketch's choice — the comment there gives S3 values.

## 7. Naming

`CyThing` is the library name (`library.properties` / `library.json`) and the
C prefix (`cything_begin()`). The public header is **per platform**:
`src/CyThingEsp32.h` for this ESP32 build; a future port to another MCU gets
its own `CyThing<Mcu>.h` rather than growing `#ifdef`s in one file. The two hook names stay `app_command_handle_line` /
`app_mqtt_command_handle`.

## 8. Plan

1. ~~TCP hook~~ · ~~MQTT hook + `mqtt_publish_response()`~~ · ~~remove `gpio/`~~ — done, uncommitted on `master`.
2. ~~Install IDF 5.5.5 next to 6.0.2, trial build, list incompatibilities~~ — done, only `provisioning.c`.
3. ~~`MBEDTLS_VERSION_MAJOR` guards in `provisioning.c`; both IDF versions build~~ — done, uncommitted.
   Still to do: hardware test of the provisioning handshake on a 5.5 build.
4. ~~`cything_begin()` + `CyThingEsp32.h` (`extern "C"`); `main/app_main.c` shrinks
   to the hooks + `app_main()` wrapper~~ — done, builds on 5.5.5 and 6.0.2.
5. ~~Move `main/` → `src/`, vendored libs into `src/`~~ — done; `main/` is
   the IDF harness and compiles `../src` (no separate `src/CMakeLists.txt`
   needed). Unused components dropped rather than vendored. Builds on 5.5.5
   and 6.0.2 from `fullclean`.
6. ~~Arduino-safe config defaults, PEMs as C arrays, `library.properties` /
   `library.json`, `examples/Basic/`, `platformio.ini`~~ — done.
   Verified: `pio run` (pioarduino 55.03.311 = arduino-esp32 3.3.11) and
   `arduino-cli compile --fqbn esp32:esp32:esp32` against the library
   installed under `~/Documents/Arduino/libraries/` both compile and link
   `examples/Basic`; `nm` shows the sketch's hooks (`T`) and the library's
   weak claim defaults (`V`). **Size: ~1.03–1.05 MB against 1 MiB app
   partitions** — see §4 "Partition table".
7. Hardware: flash the Arduino-built `examples/Basic`, confirm boot, pairing,
   TCP `relay_on` / `whoami`, MQTT `temp?`, provisioning handshake, OTA — on
   ESP32 first, then the same pass on an ESP32-S3 (§6).
8. Optional: a CyThing Arduino board package (`boards.txt` + variants with
   the partition CSVs, referencing the esp32 core) so *Tools → Board* alone
   selects flash size and table.
9. Release: `scripts/release.sh 1.0.0` (bumps `library.properties`,
   `library.json` and `FIRMWARE_VERSION` together, commits, tags, pushes —
   see [git-workflow.md](git-workflow.md) "Release"). Then either publish to
   the Arduino Library Manager (needs a public repo + the tag) /
   `pio pkg publish`, or hand out the git URL + tag. Tag every stable state
   from then on.
