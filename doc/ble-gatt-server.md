# Station-mode BLE GATT server

> **Status:** implemented, the ESP-IDF example (`on_off_pin`, IDF 6.0.3,
> esp32) builds and links the sketch's table, **not yet run on
> hardware or against the app**. The Arduino/PlatformIO build was not
> compiled (no toolchain on the dev machine at the time).

A channel can define its own BLE services and characteristics in the phone
app. Once the device is on the router (station mode), the library serves
them: the scan beacon becomes connectable, a phone connects, and the
sketch sees writes and reads through hooks and pushes values with
`cything_ble_notify()`. Values are raw bytes; the app's JavaScript decodes
them.

Without a table nothing changes: station mode stays the non-connectable scan
beacon of [ble-scan-beacon.md](ble-scan-beacon.md), no extra RAM. Pairing
(soft-AP) mode never serves the table; it runs the Wi-Fi pairing service
([ble-pairing.md](ble-pairing.md)).

## The table

Types in [src/ble/ble_app_gatt.h](../src/ble/ble_app_gatt.h) (pulled in by
`CyThingEsp32.h`):

```c
#define CYTHING_BLE_READ    0x01
#define CYTHING_BLE_WRITE   0x02
#define CYTHING_BLE_NOTIFY  0x04

typedef struct {
    const char *name;        /* unique across the table */
    uint8_t     uuid128[16]; /* NimBLE little-endian (the string read backwards) */
    uint8_t     flags;       /* CYTHING_BLE_READ | _WRITE | _NOTIFY */
} cything_ble_chr_t;

typedef struct {
    uint8_t                  uuid128[16];
    const cything_ble_chr_t *chrs;
    uint8_t                  chr_count;
} cything_ble_svc_t;
```

The library reads it through two symbols, weak `NULL` / `0` by default:

```c
extern const cything_ble_svc_t *const cy_ble_services;
extern const uint8_t                  cy_ble_service_count;
```

### What the app generates

Same split as the rest of the channel spec: **data in
`cything_network_spec.h`, the symbols in `cything_config.ino` /
`cything_config.cpp`** (which already includes `CyThingEsp32.h` first, as it
must). [examples/Basic](../examples/Basic) carries the exact shape:

```c
/* cything_network_spec.h */
#include <CyThingEsp32.h>   /* the types; angle brackets work in both builds */

static const cything_ble_chr_t CYTHING_BLE_SVC_1101_CHRS[] = {
    { "rx", { 0x18,0xE4,0x12,0x3A,0x19,0x64,0xA3,0xA1,0x78,0x4C,0x02,0xDC, 0x02,0x11, 0x48,0x6A },
      CYTHING_BLE_WRITE },
    { "tx", { 0x18,0xE4,0x12,0x3A,0x19,0x64,0xA3,0xA1,0x78,0x4C,0x02,0xDC, 0x03,0x11, 0x48,0x6A },
      CYTHING_BLE_NOTIFY },
};
static const cything_ble_svc_t CYTHING_BLE_SERVICES[] = {
    { { 0x18,0xE4,0x12,0x3A,0x19,0x64,0xA3,0xA1,0x78,0x4C,0x02,0xDC, 0x01,0x11, 0x48,0x6A },
      CYTHING_BLE_SVC_1101_CHRS, 2 },
};
#define CYTHING_BLE_SERVICE_COUNT 1

/* cything_config.ino / cything_config.cpp */
#ifdef CYTHING_BLE_SERVICE_COUNT
const cything_ble_svc_t *const cy_ble_services     = CYTHING_BLE_SERVICES;
const uint8_t                  cy_ble_service_count = CYTHING_BLE_SERVICE_COUNT;
#endif
```

- The arrays are `static`: harmless if the header is included twice, and
  only the config file turns them into linked symbols. The `#ifdef` keeps
  network specs without a table building.
- UUID bytes: the 128-bit UUID string read backwards, exactly like
  `CYTHING_BLE_BASE_UUID128_BYTES`; the 4-hex slot `xxxx` of
  `6A48xxxx-…` lands at indices 12 (low byte) and 13 (high byte). A UUID on
  the Bluetooth base `0000xxxx-0000-1000-8000-00805F9B34FB` is registered as
  the 16-bit UUID `xxxx`, as phones expect.
- Names are what the hooks receive. Keep them unique (a duplicate logs a
  warning; the first one wins in lookups).
- Limits: 16 services, 32 characteristics per service
  (`CYTHING_BLE_MAX_SERVICES`, `CYTHING_BLE_MAX_CHRS_PER_SERVICE`); larger
  tables are cut with an error in the log. A characteristic with no name or
  no flag is skipped.

### Reserved slots

In the channel base UUID the library uses:

| Slot | Use |
|---|---|
| `0x0001`–`0x0006` | Wi-Fi pairing service (pairing mode) |
| `0x0101` | scan beacon service UUID |
| `0x0110` / `0x0111` / `0x0112` | AUTH service / line link (PAKE, ENROLL, AUTH) / INFO characteristic (station mode, see below) |

**The app must keep channel services and characteristics out of
`0x0001`–`0x0006` and `0x0100`–`0x01FF`.** A channel service on `0x0110` is
skipped.

## Hooks

Declared in [CyThingEsp32.h](../src/CyThingEsp32.h), weak in the library,
run on the **NimBLE host task** (5 KB stack): short, non-blocking, no
`delay()` — a slow hook stalls every BLE connection.

```c
bool app_ble_write(const char *name, const uint8_t *data, int len);
int  app_ble_read (const char *name, uint8_t *out, int max);
bool app_ble_is_public(const char *name);
int  cything_ble_notify(const char *name, const uint8_t *data, int len);
```

| | |
|---|---|
| `app_ble_write` | a complete write (short, or long prepare/execute) to a `CYTHING_BLE_WRITE` characteristic, 0–512 bytes. `data[len]` is a NUL, so text works as a C string. `true` = accepted; `false` → ATT error `0x81` (`CYTHING_BLE_ATT_ERR_REJECTED`). Default: logs, `false`. Write *with* response only (no write-without-response), so the phone always learns a refusal. |
| `app_ble_read` | fill `out` (≤ `max` = 512) and return the length. A value longer than MTU−1 is read in pieces (ATT Read Blob) and the hook is called per piece: return the same bytes each time. Default: 0 bytes. |
| `app_ble_is_public` | the twin of `app_command_is_public()`: `true` lets anyone read / write / be notified on that characteristic with `LOCAL_AUTH_ENFORCE` on. Default `false`. |
| `cything_ble_notify` | push to every connected, subscribed phone that may see the characteristic. Any task. Returns the number of phones notified, or `CYTHING_BLE_ERR_NOT_RUNNING` (−1, no table / pairing mode), `_UNKNOWN_CHR` (−2), `_NOT_NOTIFY` (−3), `_NO_SUBSCRIBER` (−4: nobody connected, subscribed and allowed), `_NO_MEM` (−5, NimBLE buffers, retry), `_ARG` (−6). A notification carries at most MTU−3 bytes (20 on the default MTU; phones usually negotiate 185+); longer data is cut per phone with a warning. |

## Advertising

One advertisement, extended rather than alternated: the scan beacon's
payload is unchanged (scan service UUID + manufacturer data with the IP in
the advertising packet, the name in the scan response), only the PDU
changes:

| State | PDU | Manufacturer data (IP) |
|---|---|---|
| no table | `ADV_SCAN_IND` (non-connectable), only while the station has an IP — as before | yes |
| table, IP, a connection slot free | `ADV_IND` (connectable, still scannable) | yes |
| table, no IP (router down) | `ADV_IND` | **left out** — the services still work, and no phone tries a stale address |
| table, every slot taken | `ADV_SCAN_IND` | as above — the beacon keeps going |
| discovery mode Wi-Fi only (`DISCOVERYMODE:1`) | none — connected phones stay, new ones cannot connect | — |

Why not alternate two sets: the phone already filters on the scan UUID, and
a connectable `ADV_IND` with the same payload is found by the same scan; a
second set would halve each one's airtime for nothing. Legacy advertising
stops on a connection, so the beacon re-arms on every connect and
disconnect. The app connects to the device it found by the scan UUID and
then discovers the channel's services.

`DISCOVERYMODE:1` turns BLE advertising off entirely, so a channel that
relies on its BLE services should not offer Wi-Fi-only discovery.

## Security

TCP runs every protected line through the local-auth policy
([local-auth.md](local-auth.md)): with `LOCAL_AUTH_ENFORCE 1` (the shipped
setting) only a phone that proved a paired account, over an encrypted
channel, may send device commands. A GATT write is a device command, so the
server applies the same rule rather than opening a second, unauthenticated
command channel:

1. **Encrypted link.** With `BLE_APP_GATT_REQUIRE_ENCRYPTION 1` (default)
   every channel characteristic and the auth characteristic carry the
   `*_ENC` permission: the first access triggers LE Secure Connections
   pairing (Just Works — the device has no display or keys). Bonding is on
   (`BLE_APP_GATT_BONDING`), so the OS pairing dialog shows once per phone,
   not per connection; bonds persist in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`).
   A phone that lost its bond re-pairs (`REPEAT_PAIRING` deletes the old one).
2. **Which device, and is AUTH needed.** The AUTH service also has a
   read-only INFO characteristic (`6A480112-…`, since 1.4.1) returning the
   same CSV as `GET_INFO` on the LAN: the `deviceId` the account key is
   stored under, and the caps (`nopw` = open device, nothing to prove;
   `authreq`; `email`). Public, like `GET_INFO`. A phone that found the
   device over BLE reads it first.
3. **Pairing and reconnecting: the line link.** Since 1.5.0 the AUTH
   characteristic (`6A480111-…` in service `6A480110-…`, read + write) carries
   the **same lines as the TCP link** — `PAKE1..4` (pair with the device
   password, or the initial password of an open device), `ENROLL` (inside
   `ENC:` frames under `K_pake`, returns the account's `K_account`) and
   `AUTH1..3` (reconnect with `K_account`). Each BLE connection is a
   pseudo-socket in the TCP session pool (`security/local_link_io.h`), so
   `pake_handler.c`, `local_auth.c` and `enc_frame.c` serve it unchanged:
   same rate limiting (one bucket for all BLE phones, plus the global
   limit), same paired list, same pairing events, same revocation. The app
   reuses its TCP handshake code.

   Lines are longer than an attribute (512 bytes; `PAKE1` and `PAKE2` are
   ~520–550), so both directions move in chunks of up to 500 bytes:

   ```
   phone write   up to 500 bytes of the line; the last chunk ends in "\n"
   phone read    the current chunk of the reply; empty while the device is still working
   phone write   "\n" alone (no line in progress): next chunk of the reply
   ```

   A complete line starts a short-lived worker task (SRP-3072 needs ~10 KB
   of stack and a second or two — too much for the BLE host task); poll
   with reads until the reply appears. A new line discards whatever was left
   of the previous reply; a line sent while one is still being worked on
   gets ATT error `0x82` (`CYTHING_BLE_ATT_ERR_BUSY`). Only these handshake
   lines run here: owner commands (`PWSET`, `LIST`, `REVOKE`, …) stay on TCP.

   The connection counts as paired from `ENROLLED` or `AUTHOK` on, as a TCP
   socket does (`access_policy_sock_authenticated`). After `PAKE4` the
   device answers in `ENC:` frames, as on TCP; the channel's characteristic
   values never are (see below). 1.4.1's single-write `AUTH` without `\n`
   is not accepted any more.
4. **Policy per access.** Read, write and notify on a channel
   characteristic need an authenticated connection, unless
   `app_ble_is_public(name)` says otherwise, or the device is open (no
   password set: everything is public, as on TCP). A refused read / write
   gets ATT error `0x80` (`CYTHING_BLE_ATT_ERR_AUTH`); a refused phone gets
   no notifications. The account is re-checked on every access, so `REVOKE`,
   `PWSET` (non-owners) and `RESET` cut a BLE phone off at its next access.
   `LOCAL_AUTH_ENFORCE 0`: everything is accepted and the would-be refusal is
   logged at debug level, as on TCP.

Channel characteristic values are not wrapped: TCP needs `ENC:` frames
because the LAN is shared; here the LE link is already AES-CCM encrypted and
integrity-protected, so they stay raw bytes. Only the handshake lines on the
auth characteristic use `ENC:` (`ENROLL` carries the account key).

**Residual risk — accepted for now (2026-11-21).** Just Works pairing does not
authenticate the link: an attacker who is in radio range *at the moment a
phone first pairs* (or re-pairs) can sit in the middle, relay the AUTH
exchange (the proof is not bound to the link), and then inject writes on
the authenticated connection. Passive sniffing and later injection into an
existing link are not possible. The risk is accepted for the first
release, on a par with the open-device window on TCP. If it is revisited,
the options are:

- bind AUTH to the link: wrap writes in `ENC:`-style frames under a
  `K_session` derived from AUTH (breaks the "values are raw bytes"
  contract for the app's JavaScript layer, but the library could unwrap
  before the hooks);
- passkey / numeric comparison pairing, which needs a display or a printed
  per-unit code.

## Memory

Nothing extra without a table. With one, on top of the beacon's
controller + host (~60 KB, already paid by the beacon):

- the library's arrays: ~60 B per characteristic + ~30 B per service
  (heap, once);
- NimBLE's attribute table: 2 attributes per characteristic + 1 CCCD per
  NOTIFY characteristic + 1 per service, roughly 30 B each, plus a
  subscription record per NOTIFY characteristic per connection;
- per connection: controller + host state, about 1–2 KB;
- host task stack 4 → 5 KB.

Estimates from the structure sizes, not measured: a typical table (1–2
services, a handful of characteristics) costs **2–4 KB** plus ~1–2 KB per
connected phone. The 16 × 32 worst case (512 characteristics, all NOTIFY)
comes to roughly **70–80 KB**, which a classic ESP32 running Wi-Fi, TLS
and MQTT does not have to spare; **keep a channel to about 64
characteristics** on the original ESP32. Measure with the `BLE beacon +
GATT server started … free heap N` boot line.

NimBLE sizes the attribute table at runtime from what is registered
(`ble_gatts_count_cfg`), so no Kconfig limit needs raising for the table
itself.

## Config

| Setting | Where | Default | |
|---|---|---|---|
| `BLE_APP_GATT_REQUIRE_ENCRYPTION` | `ble_config.h` | 1 | LE encryption on every characteristic |
| `BLE_APP_GATT_BONDING` | `ble_config.h` | 1 | keep LE bonds |
| `BLE_APP_AUTH_SVC_ID` / `_CHR_ID` / `BLE_APP_INFO_CHR_ID` | `ble_config.h` | `0x0110` / `0x0111` / `0x0112` | |
| `LOCAL_AUTH_ENFORCE` | `device_config.h` | 1 | shared with TCP |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | `sdkconfig.defaults` | 3 (was 1) | phones at once |
| `CONFIG_BT_NIMBLE_NVS_PERSIST` | `sdkconfig.defaults` | y | bonds survive a reboot |
| `CONFIG_BT_NIMBLE_MAX_CCCDS` | `sdkconfig.defaults` | 32 (was 8) | bonded phones' stored subscriptions; an overflow evicts the oldest bond |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | `sdkconfig.defaults` | 5120 (was 4096) | the hooks run there |

The app ships its own copy of `sdkconfig.defaults` in every generated
ESP-IDF project (`FirmwareSketchBundle.IDF_SDKCONFIG_DEFAULTS`); it needs the
same four lines. Arduino builds take NimBLE-Arduino's `nimconfig.h`
defaults instead (3 connections, bonds in NVS).

`BLE_SCAN_BEACON_ENABLED 0` now only drops station-mode BLE when there is
no table; with a table the beacon + server start anyway.

## Code

| File | Role |
|---|---|
| `src/ble/ble_app_gatt.h` | public types, `cy_ble_*`, `cything_ble_notify()`, error codes; the internal API for the beacon |
| `src/ble/ble_app_gatt.c` | table → NimBLE definitions, access callbacks, AUTH, per-connection state, policy, notify, weak hook defaults |
| `src/ble/ble_beacon.c` | registers the table before the host runs; connectable advertising; forwards GAP events |
| `src/device_config/cy_config.c` | the weak `cy_ble_services = NULL` / `cy_ble_service_count = 0`. Kept out of `ble_app_gatt.c` on purpose: GCC folds a weak const scalar whose initializer is in the same file, and the sketch's table was ignored (verified by disassembly) |
| `src/cything.c` | starts the beacon when there is a table even with `BLE_SCAN_BEACON_ENABLED 0` |

## Testing

nRF Connect on a phone next to a device in station mode, flashed with
`examples/Basic` or `on_off_pin`:

- boot log: `gatt: 1 services, 2 characteristics, 3 connections max, AUTH
  enforced`, then `BLE beacon + GATT server started …`;
- the advert is now connectable; connect → services `6A481101-…` (rx, tx)
  and `6A480110-…` (auth). Advertising continues (`…, connectable`) while
  slots remain;
- with no password set on the device: write `01` to rx → the pin goes high,
  tx notifies `01` (subscribe first); writing two bytes → error `0x81`;
- with a password set and `LOCAL_AUTH_ENFORCE 1`: writing rx → error
  `0x80`; run AUTH (compute the lines with
  [scripts/local_auth_client.py](../scripts/local_auth_client.py)'s formulas
  and a known `K_account`) → `AUTHOK:…` → writes succeed; `REVOKE` the
  account over TCP → the next write fails with `0x80`;
- the first access triggers the OS pairing dialog once; reconnecting does
  not; after a device reflash the phone re-pairs cleanly;
- router off → the advert stays, without manufacturer data; rx still works.

## Open

- Revisit the accepted Just Works MITM risk (Security) before a
  security-sensitive product ships.
- `RESET` / the factory reset do not clear the LE bond store.
- Not run on hardware; heap with MQTT + TLS + a connected phone unmeasured.
- Arduino build unverified (no PlatformIO on the dev machine at the time).
