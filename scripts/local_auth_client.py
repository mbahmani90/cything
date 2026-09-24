#!/usr/bin/env python3
"""
Test client for the local-link security handshake (doc/local-auth.md).

Talks the same line protocol the phone app will, against a flashed device,
so the firmware can be exercised before the Android side exists:

    pair    PAKE1..4 with the device password (SRP-6a, matches esp_srp), then
            ENROLL this "phone" — prints K_phone, keep it for `auth`
    auth    AUTH1..3 reconnect with a K_phone (nonce/HMAC mutual auth, HKDF)
    list    (owner) print the paired list          — auth first, then LIST
    revoke  (owner) remove a paired phone          — auth first, then REVOKE:
    pwset   (owner, or anyone while no password)   — PWSET: a new password
    reset   (owner) wipe paired list + password    — auth first, then RESET

The SRP maths is ~40 lines of pow() on the RFC 5054 3072-bit group with
SHA-512, written to match esp_srp byte for byte (padding rules, hash inputs,
minimal-length big-endian integers). The only non-stdlib dependency is
`cryptography` for AES-GCM (ENC: frames) — the ESP-IDF venv already has it:
    ~/.espressif/tools/python/v6.0.2/venv/bin/python scripts/local_auth_client.py ...

Usage:
    scripts/local_auth_client.py pair <device-ip> <password> [--sub S] [--install I] [--name N]
    scripts/local_auth_client.py auth <device-ip> <k_phone-hex> [--sub S] [--install I] [--send LINE ...]
"""

import argparse
import base64
import hashlib
import hmac
import secrets
import socket
import sys

# --- SRP-6a parameters, fixed by esp_srp ------------------------------------

N_HEX = (
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"
)
N = int(N_HEX, 16)
N_LEN = 384
g = 5
IDENTITY = b"cy-device"          # DEVICE_PW_SRP_IDENTITY


def H(*parts: bytes) -> bytes:
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return h.digest()


def to_bytes(x: int) -> bytes:
    """Minimal-length big-endian, as esp_mpi_to_bin() produces."""
    return x.to_bytes((x.bit_length() + 7) // 8, "big") if x else b"\x00"


def pad(b: bytes) -> bytes:
    return b.rjust(N_LEN, b"\x00")


def srp_client_finish(a: int, A_bytes: bytes, password: bytes, salt: bytes, B_bytes: bytes):
    """Client side once the device's salt and B are known. Returns (M1, K, expected_M2)."""
    B = int.from_bytes(B_bytes, "big")
    if B % N == 0:
        raise ValueError("device sent B == 0 mod N")

    k = int.from_bytes(H(to_bytes(N), pad(to_bytes(g))), "big")          # k = H(N | PAD(g))
    u = int.from_bytes(H(pad(A_bytes), pad(B_bytes)), "big")            # u = H(PAD(A) | PAD(B))
    x = int.from_bytes(H(salt, H(IDENTITY, b":", password)), "big")     # x = H(s | H(I ":" P))
    S = pow((B - k * pow(g, x, N)) % N, a + u * x, N)
    K = H(to_bytes(S))                                                  # K = H(S), 64 bytes

    hn_xor_hg = bytes(p ^ q for p, q in zip(H(to_bytes(N)), H(pad(to_bytes(g)))))
    M1 = H(hn_xor_hg, H(IDENTITY), salt, A_bytes, B_bytes, K)          # M1 = H(H(N)^H(g) | H(I) | s | A | B | K)
    expected_M2 = H(A_bytes, M1, K)                                     # M2 = H(A | M1 | K)
    return M1, K, expected_M2


# --- line transport -----------------------------------------------------------

DIR_TO_DEV = 1
DIR_TO_APP = 2


class LineSocket:
    """Line transport. After start_encryption(key) every line goes out as an
    ENC: frame and every incoming ENC: frame is decrypted; a plaintext line
    from the device (an early ERR:) is still returned as-is."""

    def __init__(self, host: str, port: int, verbose: bool):
        self.sock = socket.create_connection((host, port), timeout=15)
        self.buf = b""
        self.verbose = verbose
        self.aead = None
        self.tx_ctr = 0
        self.rx_ctr = 0

    def start_encryption(self, key: bytes):
        try:
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        except ImportError:
            sys.exit("ENC: frames need the `cryptography` package: pip install cryptography, "
                     "or run this script with ~/.espressif/tools/python/v6.0.2/venv/bin/python")
        self.aead = AESGCM(key)
        self.tx_ctr = self.rx_ctr = 0
        if self.verbose:
            print("-- encryption on")

    def send_line(self, line: str):
        if self.verbose:
            print(f">> {line[:80]}{'…' if len(line) > 80 else ''}")
        wire = line.encode()
        if self.aead is not None:
            self.tx_ctr += 1
            nonce = DIR_TO_DEV.to_bytes(4, "big") + self.tx_ctr.to_bytes(8, "big")
            ct = self.aead.encrypt(nonce, wire, None)          # ciphertext ‖ 16-byte tag
            wire = b"ENC:" + base64.b64encode(nonce) + b"," + base64.b64encode(ct)
            if self.verbose:
                print(f"   (as ENC: frame, ctr {self.tx_ctr})")
        self.sock.sendall(wire + b"\n")

    def recv_line(self) -> str:
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("device closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        if self.aead is not None and line.startswith(b"ENC:"):
            nonce_b64, _, ct_b64 = line[4:].partition(b",")
            nonce = base64.b64decode(nonce_b64)
            direction, ctr = int.from_bytes(nonce[:4], "big"), int.from_bytes(nonce[4:], "big")
            if direction != DIR_TO_APP or ctr <= self.rx_ctr:
                raise ValueError(f"bad frame nonce: dir {direction} ctr {ctr} (last {self.rx_ctr})")
            self.rx_ctr = ctr
            line = self.aead.decrypt(nonce, base64.b64decode(ct_b64), None)
            if self.verbose:
                print(f"   (ENC: frame, ctr {ctr})")
        text = line.decode(errors="replace")
        if self.verbose:
            print(f"<< {text[:80]}{'…' if len(text) > 80 else ''}")
        return text

    def close(self):
        self.sock.close()


def b64(b: bytes) -> str:
    return base64.b64encode(b).decode()


def unb64(s: str) -> bytes:
    return base64.b64decode(s)


# --- commands -----------------------------------------------------------------

def cmd_pair(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        # PAKE1 needs A before we have B; A only depends on our private a, so
        # compute it first and rerun the maths once B arrives.
        a = secrets.randbits(256)
        A = pow(g, a, N)
        A_bytes = to_bytes(A)
        ls.send_line("PAKE1:" + b64(A_bytes))
        reply = ls.recv_line()
        if reply.startswith("ERR:"):
            print(f"pairing refused: {reply}")
            return 2
        if not reply.startswith("PAKE2:"):
            print(f"unexpected reply: {reply}")
            return 2
        salt_b64, _, B_b64 = reply[len("PAKE2:"):].partition(",")
        salt, B_bytes = unb64(salt_b64), unb64(B_b64)

        try:
            M1, K, expected_M2 = srp_client_finish(a, A_bytes, args.password.encode(), salt, B_bytes)
        except ValueError as e:
            print(f"{e}; aborting")
            return 2

        ls.send_line("PAKE3:" + b64(M1))
        reply = ls.recv_line()
        if reply.startswith("ERR:"):
            print(f"proof rejected: {reply}")
            return 3
        if not reply.startswith("PAKE4:"):
            print(f"unexpected reply: {reply}")
            return 2
        M2 = unb64(reply[len("PAKE4:"):])
        if M2 != expected_M2:
            print("device proof M2 does NOT match — wrong device or broken implementation")
            return 4

        k_pake = K[:32]
        print("pairing OK: password accepted, device proof verified")
        print(f"K_pake = {k_pake.hex()}")
        ls.start_encryption(k_pake)

        # ENROLL: identity goes only now, after the device proved itself,
        # inside the K_pake-encrypted session (ENROLLED carries K_phone).
        ls.send_line("ENROLL:" + ",".join(b64(x.encode()) for x in (args.sub, args.name, args.install)))
        reply = ls.recv_line()
        if not reply.startswith("ENROLLED:"):
            print(f"enroll failed: {reply}")
            return 5
        key_b64, _, role = reply[len("ENROLLED:"):].partition(",")
        k_phone = unb64(key_b64)
        print(f"enrolled as {role}")
        print(f"K_phone = {k_phone.hex()}   (pass to: auth {args.host} {k_phone.hex()} --sub {args.sub} --install {args.install})")
        return 0
    finally:
        ls.close()


def hmac256(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha256).digest()


def hkdf32(ikm: bytes, salt: bytes, info: bytes) -> bytes:
    prk = hmac256(salt, ikm)
    return hmac256(prk, info + b"\x01")


def authenticate(ls: LineSocket, args) -> int:
    """AUTH1..3 on an open socket; switches it to K_session. 0 on success."""
    k_phone = bytes.fromhex(args.k_phone)
    if len(k_phone) != 32:
        print("k_phone must be 32 bytes (64 hex chars)")
        return 2
    n_p = secrets.token_bytes(16)
    ls.send_line("AUTH1:" + ",".join([b64(args.sub.encode()), b64(args.install.encode()), b64(n_p)]))
    reply = ls.recv_line()
    if not reply.startswith("AUTH2:"):
        print(f"auth refused: {reply}")
        return 3
    nd_b64, _, mac_b64 = reply[len("AUTH2:"):].partition(",")
    n_d, dev_mac = unb64(nd_b64), unb64(mac_b64)
    if not hmac.compare_digest(dev_mac, hmac256(k_phone, b"dev" + n_p + n_d)):
        print("device proof does NOT verify — not the device this K_phone was enrolled on")
        return 4
    print("device proof OK")

    ls.send_line("AUTH3:" + b64(hmac256(k_phone, b"phn" + n_d + n_p)))
    reply = ls.recv_line()
    if reply != "AUTHOK":
        print(f"phone proof rejected: {reply}")
        return 5
    k_session = hkdf32(k_phone, n_p + n_d, b"cy-local-v1")
    print("authenticated")
    print(f"K_session = {k_session.hex()}")
    ls.start_encryption(k_session)
    return 0


def cmd_auth(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        rc = authenticate(ls, args)
        if rc:
            return rc
        # Anything after AUTHOK is an encrypted line — e.g. the device's own
        # commands. Replies (from the response FIFO) come back encrypted too.
        for line in args.send or []:
            ls.send_line(line)
            print(f"reply: {ls.recv_line()}")
        return 0
    finally:
        ls.close()


def cmd_list(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        rc = authenticate(ls, args)
        if rc:
            return rc
        ls.send_line("LIST")
        while True:
            reply = ls.recv_line()
            if reply.startswith("PAIREND:"):
                print(f"{reply[len('PAIREND:'):]} paired phone(s)")
                return 0
            if not reply.startswith("PAIRED:"):
                print(f"unexpected: {reply}")
                return 3
            sub, inst, name, at, role = reply[len("PAIRED:"):].split(",")
            print(f"  {role:5}  {unb64(sub).decode():36}  {unb64(inst).decode():16}  {unb64(name).decode()!r}  at={at}")
    finally:
        ls.close()


def cmd_revoke(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        rc = authenticate(ls, args)
        if rc:
            return rc
        ls.send_line("REVOKE:" + b64(args.revoke_sub.encode()) + "," + b64(args.revoke_install.encode()))
        reply = ls.recv_line()
        print(reply)
        return 0 if reply == "ACK" else 3
    finally:
        ls.close()


def cmd_pwset(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        if args.k_phone:
            rc = authenticate(ls, args)
            if rc:
                return rc
        else:
            print("no --k-phone: bootstrap PWSET (only works while the device has no password)")
        ls.send_line("PWSET:" + b64(args.new_password.encode()))
        reply = ls.recv_line()
        print(reply)
        return 0 if reply == "ACK" else 3
    finally:
        ls.close()


def cmd_unpair(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        rc = authenticate(ls, args)
        if rc:
            return rc
        ls.send_line("UNPAIR")
        reply = ls.recv_line()
        print(reply)
        return 0 if reply == "ACK" else 3
    finally:
        ls.close()


def cmd_reset(args) -> int:
    ls = LineSocket(args.host, args.port, args.verbose)
    try:
        rc = authenticate(ls, args)
        if rc:
            return rc
        ls.send_line("RESET")
        reply = ls.recv_line()
        print(reply)
        return 0 if reply == "ACK" else 3
    finally:
        ls.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-v", "--verbose", action="store_true", help="print every line on the wire")
    sub = p.add_subparsers(dest="cmd", required=True)

    def identity_args(sp):
        sp.add_argument("--port", type=int, default=1234)
        sp.add_argument("--sub", default="test-user-sub", help="Cognito sub of the pretend user")
        sp.add_argument("--install", default="pytest-install", help="install id of the pretend phone")

    sp = sub.add_parser("pair", help="PAKE1..4 with the device password, then ENROLL")
    sp.add_argument("host")
    sp.add_argument("password")
    sp.add_argument("--name", default="Python client", help="display name shown in the paired list")
    identity_args(sp)
    sp.set_defaults(func=cmd_pair)

    sp = sub.add_parser("auth", help="AUTH1..3 reconnect with a K_phone from a previous pair")
    sp.add_argument("host")
    sp.add_argument("k_phone", help="hex, as printed by pair")
    sp.add_argument("--send", action="append", metavar="LINE",
                    help="after AUTHOK, send LINE inside an ENC: frame and print the reply (repeatable)")
    identity_args(sp)
    sp.set_defaults(func=cmd_auth)

    sp = sub.add_parser("list", help="(owner) print the paired list")
    sp.add_argument("host")
    sp.add_argument("k_phone", help="the owner's K_phone, hex")
    identity_args(sp)
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("revoke", help="(owner) remove a paired phone")
    sp.add_argument("host")
    sp.add_argument("k_phone", help="the owner's K_phone, hex")
    sp.add_argument("revoke_sub", help="userSub of the entry to remove")
    sp.add_argument("revoke_install", help="installId of the entry to remove")
    identity_args(sp)
    sp.set_defaults(func=cmd_revoke)

    sp = sub.add_parser("pwset", help="set the device password (owner; or anyone while none is set)")
    sp.add_argument("host")
    sp.add_argument("new_password")
    sp.add_argument("--k-phone", dest="k_phone", default="", help="the owner's K_phone, hex (omit for bootstrap)")
    identity_args(sp)
    sp.set_defaults(func=cmd_pwset)

    sp = sub.add_parser("unpair", help="remove THIS phone's own pairing (delete from my phone)")
    sp.add_argument("host")
    sp.add_argument("k_phone", help="this phone's K_phone, hex")
    identity_args(sp)
    sp.set_defaults(func=cmd_unpair)

    sp = sub.add_parser("reset", help="(owner) wipe the paired list and the password")
    sp.add_argument("host")
    sp.add_argument("k_phone", help="the owner's K_phone, hex")
    identity_args(sp)
    sp.set_defaults(func=cmd_reset)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
