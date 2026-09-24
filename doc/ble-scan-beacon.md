# Station-mode BLE scan beacon

> **Status:** implemented, builds (PlatformIO `esp32-4mb`), **not yet run on
> hardware**. First on-device check: the `BLE beacon started … free heap`
> line, then heap over an hour with MQTT + TLS up — the beacon keeps the
> ~60 KB of Bluetooth RAM that station mode used to give back.

Once the device is on the router it keeps Bluetooth up as a **non-connectable
beacon**: it advertises a scan service UUID plus its **LAN IPv4**, and its
identity as the local name. A phone that hears it sends a **unicast
`GET_INFO`** to that address and gets the same 9-field CSV the multicast scan
returns ([udp-discovery.md](udp-discovery.md)). No BLE connection, no GATT.

Why: the app's local discovery is a multicast UDP query, and multi-AP
networks (mesh, campus, IGMP-snooping switches) drop the *query* before it
reaches the device — while the unicast *reply* path always works. The
beacon hands the app the one thing it is missing, the address, over a radio
that does not care about the Wi-Fi topology. It also covers the window right
after Wi-Fi onboarding, before the device has a cloud identity, when the
MQTT address lookup is not available either. BLE (not Classic) because iOS
only lets apps use BLE, and NimBLE is what the pairing service already runs.

The app side (scan → unicast → existing scan pipeline) is documented in the
app repo's `doc/ble-scan-beacon.md`.

## Radio contract

| | |
|---|---|
| PDU | `ADV_SCAN_IND` — non-connectable, scannable. Interval `BLE_BEACON_ADV_INTERVAL_MIN/MAX_MS` (200–300 ms) |
| Advertising packet (30 B) | `Flags` (LE General Discoverable, BR/EDR unsupported) · **128-bit scan service UUID** (`BLE_SCAN_SVC_ID` = `0x0101` in the pairing base UUID, i.e. `6A480101-DC02-4C78-A1A3-64193A12E418` by default) · **Manufacturer Specific Data** |
| Manufacturer data (7 B) | `company id` 2 B LE (`0xFFFF`, the SIG's reserved/internal id — we have no assigned one; the UUID beside it is the identifier) · `payload version` 1 B (`0x01`) · `IPv4` 4 B, `ip1..ip4` in dotted order |
| Scan response | Local name: the provisioned **`deviceId`** (`dev_…`), or **`Cy_` + the 6-hex unit suffix** (same as the soft-AP SSID / pairing name) while the device is not provisioned yet. Names over 29 bytes are cut and flagged incomplete |
| When | Only while the station holds an address: starts on `IP_EVENT_STA_GOT_IP` (restarts with the new address on a DHCP change), stops on `WIFI_EVENT_STA_DISCONNECTED`. Never in pairing (soft-AP) mode — that mode advertises the connectable pairing service instead ([ble-pairing.md](ble-pairing.md)) |

The scan service id is a *different* slot from the pairing service (`0x0001`)
on purpose: a phone scanning for pairable units never lists paired ones and
vice versa, and both derive from the channel's configurable base UUID the
same way the characteristic UUIDs do.

Trust: the beacon is an *address hint*, nothing more. Anyone can advertise
any IP; the app only acts on the UDP reply that comes back from that address,
which goes through its normal identity check (`sourceTerminalId`, blueprint
match) before a device is listed or connected.

## Code

| File | Role |
|---|---|
| `src/ble/ble_beacon.c/.h` | The beacon: builds the two payloads, starts/stops advertising on the IP events. Two flags gate it — host synced (NimBLE host task) and IP held (esp event task); a mutex keeps the stop/start pair atomic |
| `src/ble/ble_host.c/.h` | Controller + NimBLE host bring-up, host task, own-address resolution — shared with `ble_pairing.c`, which used to own it. The two modes never run in one boot |
| `src/ble/ble_nimble.h` | The NimBLE include block (Arduino vs ESP-IDF paths), shared |
| `src/ble/ble_config.h` | `BLE_SCAN_BEACON_ENABLED`, `BLE_SCAN_SVC_ID`, `BLE_BEACON_COMPANY_ID`, `BLE_BEACON_PAYLOAD_VER`, interval |
| `src/cything.c` | Station branch: `ble_beacon_start()` instead of `ble_release_memory()` |
| `src/wifi/station_mode.c` | `ble_beacon_on_ip()` on got-IP, `ble_beacon_on_ip_lost()` on disconnect |

Set `BLE_SCAN_BEACON_ENABLED 0` to get the old behaviour back (no BLE in
station mode, RAM released at boot).

## Testing

Any BLE scanner app (nRF Connect) on a phone next to a device in station
mode:

- a device advertising the service `6A480101-…`, manufacturer data
  `FF FF 01 c0 a8 01 2a` (= `192.168.1.42`), name `dev_…` or `Cy_xxxxxx`;
- pull the router cable / turn the AP off → the advert disappears within a
  second; back on → it returns with the (possibly new) address;
- serial: `beacon advertising as dev_… at 192.168.1.42` after each got-IP,
  `BLE beacon started as … free heap N` at boot — compare `N` against a
  build with `BLE_SCAN_BEACON_ENABLED 0`.

Then the app test in its own doc: the device list fills over BLE with the
phone on a different AP of the same SSID, where the multicast scan alone
finds nothing.

## Open

- **Heap on the original ESP32.** Station mode now carries the BT controller
  + NimBLE host (~60 KB) next to Wi-Fi, TLS and MQTT. Builds fine (static RAM
  unchanged); the runtime margin is the on-device check above. Fallback is
  the compile flag.
- Privacy: the beacon tells anyone in BLE range the device's LAN address
  and id. The address is only reachable from inside that LAN and every
  command still needs the local pairing password ([local-auth.md](local-auth.md)),
  but if that is not acceptable for a deployment, a time-boxed or
  button-triggered beacon is the next step.
- Always-on advertising costs ~1–2 % airtime on the shared 2.4 GHz radio;
  no throughput impact expected at 200–300 ms, to be confirmed alongside a
  local TCP stream.
