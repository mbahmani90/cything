# Flashing the ESP32

## Setup (once per shell)

```bash
cd ~/Documents/repositories/smart_device_test_gpio
source ~/.espressif/v6.0.2/esp-idf/export.sh
```

Port on this Mac is usually `/dev/cu.usbserial-0001` (check with `ls /dev/cu.*`).

## Partition table

`partitions.csv` is the 4 MB table (`partitions/cything-4MB.csv`): two 1.5 MB
OTA slots, no factory slot, `wifi_info`/`rst_count`/`boot`, the rest as
`storage`. Variants for 8/16 MB modules live in `partitions/`; build one with
`idf.py -B build-8mb -DSDKCONFIG=build-8mb/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.8mb" build`.
A device that was flashed with the old three-slot table needs one full
`idf.py flash` (not OTA) to pick up the new one — see doc/cy_thing_lib.md §4.

## Normal flash (keeps NVS — Wi-Fi creds + provisioned identity survive)

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

Use this for a firmware change on a device you don't need to re-pair.

## Erase-flash (wipes ALL NVS — device comes up factory-fresh)

```bash
idf.py -p /dev/cu.usbserial-0001 erase-flash flash monitor
```

Wipes flash entirely: Wi-Fi router credentials, **and** the provisioning
namespace (`prov`) — `deviceId`, the per-device certificate, the on-chip
operational key, `iotEndpoint`, `sourceTerminalId`. The device reboots into
soft-AP pairing and reports `provisionState = unprovisioned`.

### When you need it

- **Testing the claim-certificate / CSR handshake from scratch.** A device that
  still has a provisioned identity reports `claimed`, so the app's
  `ProvisionDeviceScreen` takes the "already paired" branch and **skips the CSR
  handshake** (and therefore the `REQID:` / `CLAIM:` step). Erase-flash forces it
  back to `unprovisioned` so the full handshake runs.
- Moving the board to a different backend / account.
- The provisioned identity in NVS is corrupt.

### Consequence

Erase-flash **orphans** the device's IoT resources on the backend — the Thing
(`dev_<ulid>`), its certificate, and the `Device` row still exist but nothing
uses them. Harmless for a test device; run `scripts/teardown-legacy-iot.mjs` /
the T3 prune scripts (CypressTerminalKmp repo) to clean up later.

The 5×-power-cycle reset (`reset_handler`) only resets Wi-Fi to AP mode — it does
**not** clear the `prov` namespace, so it can't be used to reprovision.

## Gotcha: "python is currently active … project was configured with …"

```
'…/.espressif/tools/python/v6.0.2/venv/bin/python' is currently active in the
environment while the project was configured with '…/idf6.0_py3.14_env/bin/python'.
Run 'idf.py fullclean' to start again.
```

`build/` was configured against a different Python env than the one this shell's
`export.sh` activated. Reconfigure against the active one:

```bash
idf.py fullclean
idf.py -p /dev/cu.usbserial-0001 erase-flash flash monitor
```

`fullclean` deletes `build/`, so the next command rebuilds from scratch (~3–5
min). The standard managed venv (`.espressif/tools/python/v6.0.2/venv`) is the one
to prefer — the `idf6.0_py3.14_env` was a one-off workaround for an old system
Python.

## Claim certificate (slice CC)

The per-model claim certificate + RSA key are **identical for every unit of a
device model** and are linked in as C strings, not `.pem` files: the PEMs are
pasted verbatim between the marker lines of `claim_cert.pem.h` / `claim_key.pem.h`,
which `claim_credentials.cpp` / `.ino` `#include`s into `claim_cert_pem` /
`claim_key_pem` (`aws/claim_credentials.h`). Under ESP-IDF that means a
git-ignored `main/claim_credentials.cpp` (copy the `.cpp.example`) plus the two
`.pem.h` next to it, then `idf.py reconfigure`. Empty = the claim step is
skipped. Get the PEMs from the app's *device model → Claim certificate → Add*
(one-time download). **Never commit a real key.**

Easier: the app's *device model → Download firmware sketch* hands you a sketch
with both tabs already filled, when you export it from the dialog that just
created the certificate. See the README's "Claim certificate" and "Channel and
model configuration" sections.

The AWS root CA needs none of this — it is public and compiled into
`src/device_config/root_cert_auth.c` (`root_cert_auth_pem`).
