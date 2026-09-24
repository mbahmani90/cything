#!/usr/bin/env python3
"""
Pair a CyThing device with a Wi-Fi router over BLE from this computer —
the same GATT exchange the mobile app does (doc/ble-pairing.md).

    pip install bleak
    scripts/ble_pair.py --scan                       # list pairable devices
    scripts/ble_pair.py Cy_15753C MyRouter hunter2   # pair one

Sequence: scan for the pairing service UUID, connect, subscribe to Status,
read Info (identity), write SSID, write Password, write COMMIT to Control,
then follow Status: 0x03 = trying the credentials, 0x04 = joined the router
(Info now carries the station IP — read it), 0x02 = stored (device reboots
into station mode 1 s later), 0xFF/xx = error (see ble_pairing.h; 05/06/07
are Wi-Fi failures and nothing was stored).

With BLE_PAIRING_REQUIRE_ENCRYPTION on (the default) the first write
triggers a "Just Works" pairing; macOS shows a Bluetooth pairing dialog once.
"""
import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

BASE = "6A48{:04X}-DC02-4C78-A1A3-64193A12E418"
SVC_UUID    = BASE.format(0x0001)
SSID_UUID   = BASE.format(0x0002)
PASS_UUID   = BASE.format(0x0003)
CTRL_UUID   = BASE.format(0x0004)
STATUS_UUID = BASE.format(0x0005)
INFO_UUID   = BASE.format(0x0006)

CTRL_COMMIT = b"\x01"
STATE = {0x00: "idle", 0x01: "ready", 0x02: "stored — rebooting into station mode",
         0x03: "trying the credentials", 0x04: "joined the router", 0xFF: "error"}
ERR = {0x01: "no SSID", 0x02: "flash write failed", 0x03: "bad opcode", 0x04: "busy",
       0x05: "Wi-Fi network not found", 0x06: "Wi-Fi password refused", 0x07: "Wi-Fi join timed out"}


def describe(status: bytes) -> str:
    state, detail = status[0], status[1] if len(status) > 1 else 0
    text = STATE.get(state, f"unknown 0x{state:02X}")
    if state == 0xFF:
        text += f": {ERR.get(detail, f'0x{detail:02X}')}"
    return text


async def scan(timeout: float):
    print(f"scanning {timeout:.0f} s for {SVC_UUID} ...")
    found = await BleakScanner.discover(timeout=timeout, service_uuids=[SVC_UUID], return_adv=True)
    for dev, adv in found.values():
        print(f"  {adv.local_name or dev.name or '?':<16} {dev.address}  rssi {adv.rssi}")
    if not found:
        print("  none — is the device in pairing (AP) mode?")
    return found


async def pair(name: str, ssid: str, password: str, timeout: float):
    print(f"looking for {name} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: (adv.local_name or d.name) == name and SVC_UUID.lower() in [u.lower() for u in adv.service_uuids],
        timeout=timeout,
    )
    if dev is None:
        sys.exit(f"{name} not found (scan with --scan; the device must be in pairing mode)")

    done = asyncio.Event()
    joined = asyncio.Event()
    last = bytearray()

    def on_status(_handle, data: bytearray):
        last[:] = data
        print(f"status: {data.hex()}  {describe(data)}")
        if data[0] == 0x04:
            joined.set()
        if data[0] in (0x02, 0xFF):
            done.set()

    async with BleakClient(dev) as client:
        print(f"connected to {dev.address}")
        await client.start_notify(STATUS_UUID, on_status)
        print(f"status: {(await client.read_gatt_char(STATUS_UUID)).hex()}  (initial)")
        print(f"info:   {(await client.read_gatt_char(INFO_UUID)).decode(errors='replace')}")

        print(f"writing ssid {ssid!r}")
        await client.write_gatt_char(SSID_UUID, ssid.encode(), response=True)
        print(f"writing password ({len(password)} chars)")
        await client.write_gatt_char(PASS_UUID, password.encode(), response=True)
        print("commit")
        await client.write_gatt_char(CTRL_UUID, CTRL_COMMIT, response=True)

        try:
            # trial (<= 15 s) + flash + the 1.5 s read window
            await asyncio.wait_for(done.wait(), timeout=25)
        except asyncio.TimeoutError:
            sys.exit("no final status notify within 25 s")
        if joined.is_set():
            try:
                print(f"info:   {(await client.read_gatt_char(INFO_UUID)).decode(errors='replace')}  (with station IP)")
            except Exception as e:  # the reboot may have won the race
                print(f"info:   (not readable after join: {e})")
    return last and last[0] == 0x02


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?", help="BLE name, e.g. Cy_15753C")
    ap.add_argument("ssid", nargs="?")
    ap.add_argument("password", nargs="?", default="")
    ap.add_argument("--scan", action="store_true", help="list pairable devices and exit")
    ap.add_argument("--timeout", type=float, default=8.0, help="scan timeout in seconds")
    args = ap.parse_args()

    if args.scan or not args.name:
        asyncio.run(scan(args.timeout))
        return
    if args.ssid is None:
        ap.error("ssid is required")
    ok = asyncio.run(pair(args.name, args.ssid, args.password, args.timeout))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
