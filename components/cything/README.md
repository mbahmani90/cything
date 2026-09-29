# CyThing

ESP32 smart-device firmware as an ESP-IDF component: Wi-Fi pairing over BLE or
soft-AP, TCP/UDP local control with UDP and mDNS (`_cything._tcp`) discovery,
AWS IoT MQTT with per-device certificate provisioning, and HTTPS OTA. Call `cything_begin()` from `app_main()` and
implement the two command hooks declared in `CyThingEsp32.h`.

## Getting started

There are two ways to get a CyThing project. `idf.py create-project` is not
one of them on its own: it takes a project *name* and creates an empty project
without CyThing.

### Option 1: start from the example (recommended)

```bash
idf.py create-project-from-example "mbahmani90/cything^1.2.0:on_off_pin"
```

This creates a complete, buildable project in `on_off_pin/`: the app code,
`cert/`, `sdkconfig.defaults`, `partitions.csv`, and CyThing as a dependency.
The example switches GPIO 2 with `on_cmd` / `off_cmd` over TCP or MQTT and
replies `on_res` / `off_res`. Rename the folder if you like and edit
`main/on_off_pin.c`.

### Option 2: add CyThing to an empty or existing project

```bash
idf.py create-project my_device
cd my_device
idf.py add-dependency "mbahmani90/cything^1.2.0"
```

Then:

1. Copy [sdkconfig.defaults](examples/on_off_pin/sdkconfig.defaults) and
   [partitions.csv](examples/on_off_pin/partitions.csv) from the example into
   your project root. They are required: BLE, the partition table and
   mbedTLS's CSR support all come from them (plus the mDNS options; the
   `espressif/mdns` dependency itself comes in with CyThing).
2. In your `main/` source, call `cything_begin()` from `app_main()` and
   implement the two command hooks:

   ```c
   #include "CyThingEsp32.h"

   bool app_command_handle_line(const char *line, int len, int sock)
   {
       return false;   /* TCP command not handled */
   }

   bool app_mqtt_command_handle(const char *command, int command_length)
   {
       return false;   /* MQTT command not handled */
   }

   void app_main(void)
   {
       cything_begin();
   }
   ```

3. Optionally, add the claim certificate (next section).

### Option 3: let the phone app generate it

In the Cypress Terminal app, open a device model, tap *Push sketch to GitHub*
and pick **ESP-IDF project**. The app writes the project Option 2 describes —
plus `main/cything_config.cpp` and the two generated headers carrying the
channel's network settings — into a private repository named
`<channel>_<type>_<name>` in your GitHub account (created if it does not
exist), as a branch with a pull request. Clone it and `idf.py build`.

## Claim certificate (optional)

The per-model claim certificate and private key go in a **`cert/` directory
at your project root** (next to `main/`), as two files: `claim_cert.pem.h`
and `claim_key.pem.h`. The component finds them itself, so `main/` needs no
extra source file or CMake change.

1. Copy [examples/on_off_pin/cert](examples/on_off_pin/cert) into your
   project root (a project made with `create-project-from-example` already
   has it).
2. In both files, replace the placeholder block (the `-----BEGIN` line, `...`,
   the `-----END` line) with the PEM from the app's *device model → Claim
   certificate* screen. Keep the `R"PEM(` and `)PEM"` lines.
3. `idf.py build`. The configure step prints
   `CyThing: claim certificate + key from <dir>`. Later edits to the files
   are picked up by a plain `idf.py build`.

While either file still holds the `...` placeholder, or is missing, the claim
step is skipped (with a warning if only one is filled in).

To keep the keys out of the repo entirely, put them in any directory and
point `CYTHING_CLAIM_DIR` at it, as an environment variable or a CMake
`-D` option. It is read when CMake configures, so include `reconfigure` when
you set or change it (a `-D` value is then remembered in the build's CMake
cache):

```bash
CYTHING_CLAIM_DIR=$HOME/keys/my_model idf.py reconfigure build
```

Add `cert/` to your `.gitignore`. **Never commit a real key.**

Full documentation: <https://github.com/mbahmani90/cything>
