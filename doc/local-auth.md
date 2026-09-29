# Local-link security: password pairing, per-account keys, encrypted lines

How a phone earns the right to talk to this device over the **local link**
(the TCP command server, port 1234 — [tcp-server.md](tcp-server.md)) and
keeps it without a password or a cloud round-trip. The design and its rationale live in the app repo
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
PAKE1:<A>            ->            AUTH1:<sub>,<N_p>             ->
             <-  PAKE2:<salt>,<B>               <-  AUTH2:<N_d>,<dev proof>
PAKE3:<M1>           ->            AUTH3:<phone proof>            ->
             <-  PAKE4:<M2>                     <-  AUTHOK
  ── K_pake, ENC: from here ──        ── K_session, ENC: from here ──
ENC{ENROLL:<sub>,<email>}          ->  ENC{on_cmd} ->
             <-  ENC{ENROLLED:<K_account>,<role>} <-  ENC{12:on_res}
```

- The **device password** (8–64 chars) is proven with SRP-6a: it never
  crosses the wire, in any form. The device stores only a salt + verifier.
- **`K_account`** is a 32-byte random secret the device mints per **account**
  (userSub) at its first `ENROLL` and keeps in its paired list. Every phone of
  the account uses the same key: the app escrows it in the backend, so a
  second phone fetches it instead of pairing again. Reconnects prove it with
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
| `paired_list.{c,h}` | NVS blob `cy_sec/paired`: up to 16 × `{userSub, userEmail, K_account, pairedAt, role}`, one per account (`userEmail` = the owner's label in `LIST`). Blob v3; a v2 blob (with `installId` / `displayName`) is migrated on boot, `displayName` becoming the label. Role follows `owner_sub` (the first account to pair). |
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
dev -> PAKE2:<b64 salt>,<b64 B>           B = k·v + g^b        | ERR:LOCKED,<s> | ERR:BADFMT | ERR:NOPW
app -> PAKE3:<b64 M1>                     M1 = H(H(N)⊕H(PAD(g)) ‖ H(I) ‖ s ‖ A ‖ B ‖ K)
dev -> PAKE4:<b64 M2>                     M2 = H(A ‖ M1 ‖ K)   | ERR:BADPW | ERR:SEQ | ERR:BADFMT
```

with `k = H(N ‖ PAD(g))`, `u = H(PAD(A) ‖ PAD(B))`, `x = H(s ‖ H(I ":" P))`,
`K = H(S)` (64 bytes). After `PAKE4` both sides hold **`K_pake` = the first
32 bytes of `K`** and the socket is in `LS_PAKE_OK`.
[scripts/local_auth_client.py](../scripts/local_auth_client.py) is the
byte-exact reference for these formulas.

`ERR:LOCKED,<seconds>` — rate limited. With no password stored, `P` is the
model's initial password (see *Open device*); `ERR:NOPW` means its verifier
could not be derived. A fresh `PAKE1`/`AUTH1` always restarts the socket
(local_session_reset_handshake), so a half-finished handshake never wedges it.

### Enroll — inside the `K_pake` session

```
app -> ENC{ ENROLL:<b64 userSub>,<b64 userEmail> }
dev -> ENC{ ENROLLED:<b64 K_account>,<owner|user> }    | ERR:SEQ | ERR:FULL | ERR:BADFMT
```

**One key per account.** A new `userSub` gets a fresh `K_account` and a new
entry. A `userSub` that is already paired — from this phone or another — gets
its **existing** `K_account` back; the key is never rotated by `ENROLL`, so
two phones of one account pairing at once end up with the same key and never
lock each other out, and the copy escrowed in the backend stays valid. The
lookup is atomic (`paired_list_add` under the list mutex).

This holds on an **open device** too: there anyone can `PAKE` with the public
initial password and the `userSub` is only asserted, so whoever knows the
owner's sub can get the owner's key while the device is open — as they can
control the device anyway. `PWSET` therefore **rotates the owner account's
key**: a key handed out while the device was open dies when the owner locks
it.

Older apps send `ENROLL:<sub>,<displayName>,<installId>[,replace]`; that is
still accepted — `installId` and `,replace` are dropped and `displayName` is
stored as the label until the app enrolls with an email.

Identity travels only here, after the device proved itself in `PAKE4`.
`ERR:SEQ` if not in `LS_PAKE_OK` or not sent as an `ENC:` frame.
Re-enrolling updates the entry's `userEmail` and `pairedAt`; key and role
stay. Field limits: sub 40, email 64 bytes, printable ASCII.

`userEmail` is only a label for the owner (so `LIST` reads `alice@x.com`
rather than a UUID). The device cannot verify it — the app asserts it, like
the sub on an open device — and it goes stale if the account's email
changes, until the next `ENROLL`. `userSub` stays the identity. It is PII in
flash: `RESET` wipes it with the rest of the list, and it is not sent in
pairing events.

### Reconnect — `AUTH1..3`

```
app -> AUTH1:<b64 userSub>,<b64 N_p>                           N_p: 16 random bytes
dev -> AUTH2:<b64 N_d>,<b64 HMAC-SHA256(K_account, "dev" ‖ N_p ‖ N_d)>   | ERR:UNKNOWN | ERR:BADFMT
app -> AUTH3:<b64 HMAC-SHA256(K_account, "phn" ‖ N_d ‖ N_p)>
dev -> AUTHOK:<owner|user>                                       | ERR:BADAUTH | ERR:SEQ
```

The entry is looked up by `userSub` (older apps' `AUTH1:<sub>,<installId>,<N_p>`
is still accepted, `installId` ignored). The role is the account's entry in the paired list, as `ENROLLED` reports it:
a reconnecting phone needs it to know whether the owner commands are its to
send, and `LIST` (which would tell it) is owner-only.

`K_session = HKDF-SHA256(ikm = K_account, salt = N_p ‖ N_d, info =
"cy-local-v1")`, 32 bytes (extract + one expand block). `ERR:UNKNOWN` is
what a revoked account sees. Any number of sockets may authenticate with
the same `K_account` at once; each has its own nonces, so its own `K_session`.

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
dev -> PAIRED:<b64 userSub>,<b64 userEmail>,<pairedAt>,<owner|user>   × N
dev -> PAIREND:<N>

app -> REVOKE:<b64 userSub>[,<b64 installId>]   (installId ignored)
dev -> ACK                       | ERR:UNKNOWN | ERR:OWNER | ERR:BADFMT | ERR:STORE

app -> PWSET:<b64 newPassword>   (8–64 bytes)
dev -> ACK                       | ERR:BADFMT | ERR:STORE
  Every non-owner account is revoked and the owner account's key is rotated;
  the caller's session keeps working, and the app re-enrolls with the new
  password on the same socket to get (and escrow) the new key.

app -> UNPAIR                    (any authenticated session)
dev -> ACK                       | ERR:OWNER | ERR:AUTH | ERR:UNKNOWN
  Removes the CALLER's own account entry ("delete this device from my
  account") — every phone of the account loses local access — and publishes
  an `unpaired` event. USER accounts only: the owner gets `ERR:OWNER` and
  stays paired. Ownership belongs to the account that onboarded the device
  (the first to ENROLL after a factory reset) and never moves to another
  account; only `RESET` or the power-cycle factory reset clears it. The app's
  "Delete device" on the owner's phone therefore only hides the device.

app -> RESET
dev -> ACK                       (then the list + password are wiped, every session de-authenticated)

app -> DISCOVERYMODE:<1|2|3>     1 = Wi-Fi only, 2 = BLE beacon, 3 = both (doc/ble-scan-beacon.md)
dev -> ACK                       | ERR:BADFMT | ERR:STORE
  Persisted (NVS `cy_ble`/`disc_mode`) and applied at once: the scan beacon
  stops or starts. mDNS and the unicast GET_INFO server run in every mode.

app -> DISCOVERYMODE?
dev -> DISCOVERYMODE:<1|2|3>     the mode currently applied
```

Non-owner → `ERR:AUTH`. Revoking an account drops every one of its sessions
that is connected right now. `REVOKE` of the caller's OWN entry is refused
(`ERR:OWNER`) so the device is never left without a manager — that is
`UNPAIR`. `PWSET` revokes every non-owner account (they re-pair with the new
password); the owner account is kept. A stolen phone means revoking the whole
account and pairing again: that mints a new `K_account`, which the app escrows
in place of the old one. For the owner (who cannot unpair) it is `PWSET`, which
rotates the owner key.

### Open device (no password stored)

A fresh unit, one after `RESET` / the power-cycle factory reset, or one the
owner opened with `PWCLEAR` has no password: it is **open to all**, every
line is public, and `pake|nopw` in the scan reply's caps says so.

`PAKE` still works in this state, with the model's **initial password**
(`cy_initial_password`, default `12345678`, set per model from
`CYTHING_INITIAL_PASSWORD` in `cything_device_params.h`; the app reads the same
value from the DefinedDevice). It is public — it does not authenticate
anyone — but SRP still gives the socket a key an eavesdropper cannot derive,
so nothing after `PAKE4` travels in plaintext. No rate limiting applies.

- **First phone:** `PAKE` (initial password) → `ENROLL` → becomes the owner
  → `PWSET` inside the same `ENC:` session. The password never crosses the
  LAN in plaintext.
- **Any later phone:** `PAKE` (initial password) → `ENROLL` → `user`.
  Its entry lasts until the owner sets a password: `PWSET` revokes every
  user, and from then on new phones pair with the real password.
- **An already-paired account from another phone** (or a reinstalled one):
  `ENROLL` → its existing `K_account` and role. Normally the app fetches the
  key from the backend escrow and goes straight to `AUTH` instead.

Accepted risk: `userSub` is asserted, not proven, while the device is open, so
anyone on the LAN who knows the owner's sub gets the owner's key (and role) —
until the owner's `PWSET`, which rotates that key. An open
device is controllable by anyone on the LAN anyway; setting a password closes
the window, since `PAKE` then needs it.

`PWSET` is owner-only in every state; there is no plaintext bootstrap.
An active man-in-the-middle who knows the initial password can still
impersonate the device during that first `PWSET`; a per-unit password
(label / QR) would close that.

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
hash; the `K_account`s are directly usable. **Enable flash encryption +
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
{"event":"paired","userSub":"…","role":"user","at":1789664982}
```

Over the device's own mutual-TLS session, so the backend may trust it and
grant/remove remote (`DeviceAccess`) rights accordingly. The per-device IoT
policy must allow `iot:Publish` on that topic.

## Scan reply

`provisioningCaps` (field 9) is now `|`-separated: `pake` always,
`nopw` while no password is stored (PAKE with the initial password), `authreq` when `LOCAL_AUTH_ENFORCE`
is 1, `claim` as before. See [udp-discovery.md](udp-discovery.md).

## Config

| Define | Where | Default | Meaning |
|---|---|---|---|
| `LOCAL_AUTH_ENFORCE` | device_config.h | 0 | apply the access policy |
| `LOCAL_SESSION_MAX` | local_session.h | 8 | concurrent local sockets |
| `PAIRED_LIST_MAX` | paired_list.h | 16 | paired accounts |
| `DEVICE_PW_MIN_LEN` / `MAX_LEN` | device_password.h | 8 / 64 | |
| `cy_initial_password` | cything_device_params.h (`CYTHING_INITIAL_PASSWORD`) | `12345678` | PAKE password while none is set |
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
$PY scripts/local_auth_client.py -v pair <ip> 12345678                  # open device: PAKE + ENROLL -> K_account (owner)
$PY scripts/local_auth_client.py pwset <ip> testpass123 --k-account <K_account>   # owner locks it
$PY scripts/local_auth_client.py auth <ip> <K_account> --send on_cmd      # AUTH + an encrypted command
$PY scripts/local_auth_client.py pair <ip> testpass123 --sub bob --email bob@example.com
$PY scripts/local_auth_client.py pair <ip> testpass123 --sub bob --email bob@example.com   # a 2nd phone: same K_account
$PY scripts/local_auth_client.py list <ip> <owner K_account>                                # shows bob@example.com
$PY scripts/local_auth_client.py revoke <ip> <owner K_account> bob
$PY scripts/local_auth_client.py reset <ip> <owner K_account>
```

Expected failures worth trying: wrong password → `ERR:BADPW`, six in a row
→ `ERR:LOCKED,60`; `auth` after `revoke` → `ERR:UNKNOWN`; with
`LOCAL_AUTH_ENFORCE 1`, a bare `on_cmd` on a fresh socket → `ERR:AUTH`.

## Not done here

- Android/iOS client (the app repo).
- Backend: IoT rule + `pairing-bridge` Lambda, IoT policy for the
  `pairing` topic.
- Key escrow: the app stores `K_account` in an owner-only, KMS-encrypted
  backend record after `ENROLLED`; the account's other phones fetch it.
- Handshake idle timeout: a `PAKE1` never followed by `PAKE3` keeps its
  ~2 KB SRP context until the socket closes.
- Flash encryption / secure boot.
