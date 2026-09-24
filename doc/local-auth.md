# Local-link security: password pairing, per-phone keys, encrypted lines

How a phone earns the right to talk to this device over the **local link**
(the TCP command server, port 1234 — [tcp-server.md](tcp-server.md)) and
keeps it without a password, a cloud round-trip, or any secret shared
between phones. The design and its rationale live in the app repo
(`CypressTerminalKmp85327491/doc/local-pairing-password.md`); this page is
the firmware reference: wire protocol, modules, storage, config, testing.

> **Status:** implemented on the firmware side, `LOCAL_AUTH_ENFORCE 0` by
> default (everything still works unpaired; the handshake is merely
> available). Not yet exercised against a board or the app. Flip the flag
> once the pairing app is out.

## In one screen

```
first phone / new phone            every later connection
──────────────────────             ──────────────────────
PAKE1:<A>            ->            AUTH1:<sub>,<install>,<N_p>   ->
             <-  PAKE2:<salt>,<B>               <-  AUTH2:<N_d>,<dev proof>
PAKE3:<M1>           ->            AUTH3:<phone proof>            ->
             <-  PAKE4:<M2>                     <-  AUTHOK
  ── K_pake, ENC: from here ──        ── K_session, ENC: from here ──
ENC{ENROLL:<sub>,<name>,<install>} ->  ENC{on_cmd} ->
             <-  ENC{ENROLLED:<K_phone>,<role>}   <-  ENC{12:on_res}
```

- The **device password** (8–64 chars) is proven with SRP-6a: it never
  crosses the wire, in any form. The device stores only a salt + verifier.
- **`K_phone`** is a 32-byte random secret the device mints per phone at
  `ENROLL` and keeps in its paired list. Reconnects prove it with
  nonce/HMAC, both ways, device first.
- Every line after `PAKE4` / `AUTHOK` — both directions — is an **`ENC:`
  frame** (AES-256-GCM). Plaintext still works for un-paired sockets and
  public commands.
- **Access is per command**: `GET_INFO`, the handshake and whatever
  `app_command_is_public()` allows need no pairing; everything else needs
  a paired phone and arrives encrypted.

## Modules

All under [src/security/](../src/security/), wired in by
[cything_begin()](../src/cything.c) and [tcp_dispatch_line()](../src/tcp_server/tcp_command.c).

| File | Owns |
|---|---|
| `local_session.{c,h}` | one slot per socket: framing count, auth state machine, SRP handle, current key, nonces, GCM counters, paired-list index, role. Acquired on `accept`, released (keys wiped) on close. Never moves — the fix for the old global `tcp_rec_data_counter`. |
| `device_password.{c,h}` | NVS `cy_sec/pw_salt` + `pw_ver` (SRP verifier), `set/clear/get`, guess rate limiting (per IP in RAM, global persisted). |
| `paired_list.{c,h}` | NVS blob `cy_sec/paired`: up to 16 × `{userSub, installId, displayName, K_phone, pairedAt, role}`. First entry = owner. |
| `pake_handler.{c,h}` | `PAKE1..4` over `esp_srp`. Also `security_send_line()`, the direct-send helper the handshake replies use. |
| `local_auth.{c,h}` | `ENROLL`, `AUTH1..3`; HMAC-SHA256 / HKDF-SHA256 helpers on `mbedtls_md`. |
| `enc_frame.{c,h}` | `ENC:` framing: unwrap on receive (`tcp_client_recv.c`), wrap on send (`tcp_client_list.c` send task and `security_send_line`). PSA AEAD on mbedTLS 4 / `mbedtls_gcm` on 3. |
| `access_policy.{c,h}` | handshake / public / protected classification, `LOCAL_AUTH_ENFORCE`, `app_command_is_public()` hook. |
| `owner_commands.{c,h}` | `LIST`, `REVOKE:`, `PWSET:`, `RESET`. |
| `pairing_events.{c,h}` | NVS queue of `paired` / `unpaired` / `reset` events, published by `aws_iot_task` on `<base>/pairing`. |

Dispatch order in `tcp_dispatch_line`: **policy → PAKE → ENROLL/AUTH →
owner commands → provisioning → OTA → Wi-Fi pairing →
`app_command_handle_line()`**.

## Wire protocol

Line-oriented, `\n`-terminated, base64 payloads, on the existing socket.
Errors are `ERR:<CODE>[,<detail>]`. Everything from the device after
`PAKE4` / `AUTHOK` is an `ENC:` frame (see below), including errors.

### Pair — `PAKE1..4`

SRP-6a as implemented by ESP-IDF's `esp_srp` (also in the Arduino prebuilt
libs): **RFC 5054 3072-bit group, g = 5, SHA-512**, identity `I =
"cy-device"` (`DEVICE_PW_SRP_IDENTITY`). These are fixed by `esp_srp`; the
phone must match them exactly. Integers are minimal-length big-endian.

```
app -> PAKE1:<b64 A>                      A = g^a
dev -> PAKE2:<b64 salt>,<b64 B>           B = k·v + g^b        | ERR:NOPW | ERR:LOCKED,<s> | ERR:BADFMT
app -> PAKE3:<b64 M1>                     M1 = H(H(N)⊕H(PAD(g)) ‖ H(I) ‖ s ‖ A ‖ B ‖ K)
dev -> PAKE4:<b64 M2>                     M2 = H(A ‖ M1 ‖ K)   | ERR:BADPW | ERR:SEQ | ERR:BADFMT
```

with `k = H(N ‖ PAD(g))`, `u = H(PAD(A) ‖ PAD(B))`, `x = H(s ‖ H(I ":" P))`,
`K = H(S)` (64 bytes). After `PAKE4` both sides hold **`K_pake` = the first
32 bytes of `K`** and the socket is in `LS_PAKE_OK`.
[scripts/local_auth_client.py](../scripts/local_auth_client.py) is the
byte-exact reference for these formulas.

`ERR:LOCKED,<seconds>` — rate limited. `ERR:NOPW` — no password stored;
see *Bootstrap*. A fresh `PAKE1`/`AUTH1` always restarts the socket
(local_session_reset_handshake), so a half-finished handshake never wedges it.

### Enroll — inside the `K_pake` session

```
app -> ENC{ ENROLL:<b64 userSub>,<b64 displayName>,<b64 installId> }
dev -> ENC{ ENROLLED:<b64 K_phone>,<owner|user> }      | ERR:SEQ | ERR:FULL | ERR:BADFMT
```

Identity travels only here, after the device proved itself in `PAKE4`.
`ERR:SEQ` if not in `LS_PAKE_OK` or not sent as an `ENC:` frame.
Re-enrolling the same `(userSub, installId)` replaces the key, keeps the
role. Field limits: sub 40, name 32, install 32 bytes, printable ASCII.

### Reconnect — `AUTH1..3`

```
app -> AUTH1:<b64 userSub>,<b64 installId>,<b64 N_p>           N_p: 16 random bytes
dev -> AUTH2:<b64 N_d>,<b64 HMAC-SHA256(K_phone, "dev" ‖ N_p ‖ N_d)>   | ERR:UNKNOWN | ERR:BADFMT
app -> AUTH3:<b64 HMAC-SHA256(K_phone, "phn" ‖ N_d ‖ N_p)>
dev -> AUTHOK:<owner|user>                                       | ERR:BADAUTH | ERR:SEQ
```

The role is this phone's entry in the paired list, as `ENROLLED` reports it:
a reconnecting phone needs it to know whether the owner commands are its to
send, and `LIST` (which would tell it) is owner-only.

`K_session = HKDF-SHA256(ikm = K_phone, salt = N_p ‖ N_d, info =
"cy-local-v1")`, 32 bytes (extract + one expand block). `ERR:UNKNOWN` is
what a revoked phone sees.

### `ENC:` frames

```
ENC:<b64 nonce12>,<b64 AES-256-GCM(key, nonce12, line) ‖ tag16>
```

- `line` is the original line **without** its trailing `\n`.
- `nonce12 = dir(4 bytes, big-endian) ‖ counter(8 bytes, big-endian)`,
  `dir` = `1` app→dev, `2` dev→app, counter starts at 1 and only increases
  per direction; the receiver rejects a counter ≤ the last accepted one
  (replay) or the wrong direction (reflection) before decrypting.
- No AAD. Tag 16 bytes appended to the ciphertext.
- Max plaintext per frame `ENC_FRAME_PLAIN_MAX` = 768 bytes.
- A bad frame gets a plaintext `ERR:ENC` and is dropped; the session
  survives.

The key is `K_pake` between `PAKE4` and the socket closing, or
`K_session` after `AUTHOK`. `PAKE4` and `AUTHOK` themselves are plaintext.

### Owner commands — owner session, `ENC:` only

```
app -> LIST
dev -> PAIRED:<b64 userSub>,<b64 installId>,<b64 displayName>,<pairedAt>,<owner|user>   × N
dev -> PAIREND:<N>

app -> REVOKE:<b64 userSub>,<b64 installId>
dev -> ACK                       | ERR:UNKNOWN | ERR:OWNER | ERR:BADFMT | ERR:STORE

app -> PWSET:<b64 newPassword>   (8–64 bytes)
dev -> ACK                       | ERR:BADFMT | ERR:STORE

app -> UNPAIR                    (any authenticated session)
dev -> ACK                       | ERR:AUTH | ERR:UNKNOWN
  Removes the CALLER's own entry ("delete this device from my phone"),
  whatever its role, and publishes an `unpaired` event. If the owner
  unpairs while other phones remain, entry 0 is promoted to owner so the
  device stays manageable.

app -> RESET
dev -> ACK                       (then the list + password are wiped, every session de-authenticated)

app -> DISCOVERYMODE:<1|2|3>     1 = Wi-Fi multicast, 2 = BLE beacon, 3 = both (doc/ble-scan-beacon.md)
dev -> ACK                       | ERR:BADFMT | ERR:STORE
  Persisted (NVS `cy_ble`/`disc_mode`) and applied at once: the scan beacon
  stops or starts, and the UDP server leaves or joins the multicast group
  (a unicast GET_INFO is answered in every mode).

app -> DISCOVERYMODE?
dev -> DISCOVERYMODE:<1|2|3>     the mode currently applied
```

Non-owner → `ERR:AUTH`. Revoking a phone that is connected right now
drops its session on the spot. `REVOKE` of the caller's OWN entry is refused
(`ERR:OWNER`) so the device is never left without a manager — that is
`UNPAIR`; any other entry may go, including the owner account's other
installs. `PWSET` leaves the paired list alone.

### Bootstrap (no password stored)

A fresh unit, or one after `RESET` / the power-cycle factory reset, has no
password. In that state every line is public (nothing *can* be protected)
and **`PWSET:` is open to any socket** — that is how the first password
gets in over TCP. The phone that sets it should immediately `PAKE` +
`ENROLL` with it and thereby become the owner. `pake|nopw` in the scan
reply's caps announces this state.

## Access policy

| Class | Members | Un-paired socket | Paired socket |
|---|---|---|---|
| handshake | `PAKE1/3`, `AUTH1/3`, `ENROLL` | ✅ | ✅ |
| public | `GET_INFO`, `app_command_is_public()` → true, everything while no password is stored | ✅ plaintext | ✅ plaintext or `ENC:` |
| protected | all else: OTA, provisioning, Wi-Fi pairing, app commands (`on_cmd`/`off_cmd` among them), owner commands | `ERR:AUTH` | `ENC:` only, else `ERR:ENC` |

`LOCAL_AUTH_ENFORCE 0` ([device_config.h](../src/device_config/device_config.h)):
the verdict is only logged (debug) and everything is accepted as before.
`1`: refusals as in the table. Owner commands check the owner role inside
regardless of the flag.

Broadcasts: `send_data_to_clients(SEND_TO_ALL, …)` reaches every **paired**
socket when enforcing (an un-paired socket may only be present for a
public command and must not see a paired phone's data); `SEND_TO_ALL_PUBLIC`
reaches everyone — use it for the reply to a public command, or
`send_raw_to_client(sock, …)`.

## Storage (NVS namespace `cy_sec`)

| Key | Type | Content |
|---|---|---|
| `pw_salt` | blob 16 | SRP salt |
| `pw_ver` | blob ≤384 | SRP verifier `g^x mod N` |
| `pw_fails` | u8 | consecutive global bad proofs |
| `pw_lock` | u32 | global lock seconds left at last write (resumes after reboot) |
| `paired` | blob | header + `count` × `paired_entry_t` (≈150 B each) |
| `pevents` | blob | header + `count` × 256 B JSON events awaiting publish |

Flash dump exposure: the verifier still has to be cracked like a password
hash; the `K_phone`s are directly usable. **Enable flash encryption +
secure boot on production units** — build-config, not covered here.

## Rate limiting

A password can only be tested by running the exchange against this device
(that is what SRP buys), so `PAKE3` failures are metered
([device_password.c](../src/security/device_password.c)):

- per source IP: 5 consecutive failures → 60 s lock, doubling per further
  failure, cap 1 h; RAM only, 8 IPs tracked. One hostile phone cannot lock
  everyone out.
- global: 20 consecutive failures → same curve; persisted.
- a successful proof clears both for that IP.

## Pairing events (MQTT)

`ENROLL` → `paired`, `REVOKE` → `unpaired`, `RESET` / factory reset →
`reset`, each queued in NVS and published by `aws_iot_task` (one per
second) on `<sourceTerminalId>/<deviceType>/<deviceId>/pairing`:

```json
{"event":"paired","userSub":"…","installId":"…","role":"user","at":1789664982}
```

Over the device's own mutual-TLS session, so the backend may trust it and
grant/remove remote (`DeviceAccess`) rights accordingly. The per-device IoT
policy must allow `iot:Publish` on that topic.

## Scan reply

`provisioningCaps` (field 9) is now `|`-separated: `pake` always,
`nopw` while no password is stored, `authreq` when `LOCAL_AUTH_ENFORCE`
is 1, `claim` as before. See [udp-discovery.md](udp-discovery.md).

## Config

| Define | Where | Default | Meaning |
|---|---|---|---|
| `LOCAL_AUTH_ENFORCE` | device_config.h | 0 | apply the access policy |
| `LOCAL_SESSION_MAX` | local_session.h | 8 | concurrent local sockets |
| `PAIRED_LIST_MAX` | paired_list.h | 16 | paired phones |
| `DEVICE_PW_MIN_LEN` / `MAX_LEN` | device_password.h | 8 / 64 | |
| `ENC_FRAME_PLAIN_MAX` | enc_frame.h | 768 | longest encryptable line |
| `TCP_RESPONSE_SEND_TASK_STACK_SIZE` | task_config.h | 5120 | raised from 3072 for the frame buffers |

mbedTLS: 4.x (ESP-IDF 6.0) → PSA AEAD; 3.x (ESP-IDF 5.5, Arduino) →
`mbedtls_gcm_*`. HMAC/HKDF are hand-built on `mbedtls_md` so no HKDF
config option is needed. `protocomm` is in `REQUIRES` for `esp_srp`.

## Testing from a computer

[scripts/local_auth_client.py](../scripts/local_auth_client.py) speaks the
whole protocol (needs `cryptography` for AES-GCM — the IDF venv has it):

```
PY=~/.espressif/tools/python/v6.0.2/venv/bin/python
$PY scripts/local_auth_client.py pwset <ip> testpass123                 # bootstrap: first password
$PY scripts/local_auth_client.py -v pair <ip> testpass123               # PAKE + ENROLL -> prints K_phone (this run = owner)
$PY scripts/local_auth_client.py auth <ip> <K_phone> --send on_cmd      # AUTH + an encrypted command
$PY scripts/local_auth_client.py pair <ip> testpass123 --sub bob --install bobphone --name "Bob"
$PY scripts/local_auth_client.py list <ip> <owner K_phone>
$PY scripts/local_auth_client.py revoke <ip> <owner K_phone> bob bobphone
$PY scripts/local_auth_client.py reset <ip> <owner K_phone>
```

Expected failures worth trying: wrong password → `ERR:BADPW`, six in a row
→ `ERR:LOCKED,60`; `auth` after `revoke` → `ERR:UNKNOWN`; with
`LOCAL_AUTH_ENFORCE 1`, a bare `on_cmd` on a fresh socket → `ERR:AUTH`.

## Not done here

- Android/iOS client (the app repo).
- Backend: IoT rule + `pairing-bridge` Lambda, IoT policy for the
  `pairing` topic.
- Key escrow for a second phone on the same account (design: opt-in,
  owner-only record in the backend).
- Handshake idle timeout: a `PAKE1` never followed by `PAKE3` keeps its
  ~2 KB SRP context until the socket closes.
- Flash encryption / secure boot.
