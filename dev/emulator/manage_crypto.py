"""manage_crypto.py — RSA + RC4 for the ECSP manage/adopt channel (dev/adopt-handshake.md).

The device wraps a random RC4 session key with the controller's RSA PUBLIC key (we derive it from
the recovered private key in dev/findings-secrets.local); the controller replies RC4'd + RSA-signed
(SHA1withRSA), which we verify. Both primitives matched to the controller's own impl:
  - RC4Utils: standard RC4 (KSA with key-repeat, PRGA) — verified vs the canonical vector.
  - RsaCipher: KEY_ALGORITHM "RSA" (→ RSA/ECB/PKCS1), SIGNATURE_ALGORITHM "SHA1withRSA".
"""
import base64
import hashlib
import os

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding


def _sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest().upper()     # CipherUtils.bytesToHexString → UPPERCASE

def _md5_hex(b: bytes) -> str:
    return hashlib.md5(b).hexdigest().upper()


def ecsp2_auth(username: str, password: str, random_key: str) -> str:
    """EcspUtils.calculateEcsp2Auth: SHA256(SHA256(username + md5(password)) + randomKey), UPPER hex.
    Factory device authenticates as admin/admin before the controller pushes it a real account."""
    inner = _sha256_hex((username + _md5_hex(password.encode())).encode())
    return _sha256_hex((inner + random_key).encode())

def ecsp2_auth_md5(username: str, md5_hex: str, random_key: str) -> str:
    """Same as ecsp2_auth but the caller already holds MD5(password) as hex (which is what the
    controller's ADOPT_REQUEST delivers: username + 0x00 + MD5(password))."""
    inner = _sha256_hex((username + md5_hex).encode())
    return _sha256_hex((inner + random_key).encode())


_SECRETS = os.path.join(os.path.dirname(__file__), "..", "findings-secrets.local")
_RC4KEY = os.path.join(os.path.dirname(__file__), "..", "rc4key.local")

import json as _json
import struct as _struct


def _fixed_rc4_key() -> bytes:
    return open(_RC4KEY).read().strip().encode()     # RC4Utils.getEncryptKey() — v2 channel framing


def rc4_frame(obj) -> bytes:
    """v2 manage-channel frame: RC4(4-byte BE len) + RC4(payload), each a fresh keystream from the
    fixed key (RC4Utils.crypt re-inits per call). Matches TcpEncryptInternalEncoder."""
    key = _fixed_rc4_key()
    payload = _json.dumps(obj, separators=(",", ":")).encode()
    # length field = TOTAL frame length (4-byte prefix + payload) — decoder does i3 = len - stripBytes(4)
    return rc4(_struct.pack(">I", 4 + len(payload)), key) + rc4(payload, key)


def rc4_unframe(sock_recv_all):
    """Read one RC4'd v2 frame. sock_recv_all(n) must return exactly n bytes (or b'')."""
    key = _fixed_rc4_key()
    lb = sock_recv_all(4)
    if len(lb) < 4:
        return None
    total = _struct.unpack(">I", rc4(lb, key))[0]
    n = total - 4                                    # length field includes the 4-byte prefix
    if n <= 0 or n > 2_000_000:
        return {"_badlen": total}
    body = sock_recv_all(n)
    try:
        return _json.loads(rc4(body, key).decode("utf-8", "replace"))
    except Exception:
        return {"_raw": rc4(body, key)[:200].decode("latin-1", "replace")}


# ── RC4 (standard; matches RC4Utils.initKey/doCrypt) ──────────────────────────
def rc4(data: bytes, key: bytes) -> bytes:
    S = list(range(256))
    j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(len(data))
    i = j = 0
    for n in range(len(data)):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out[n] = data[n] ^ S[(S[i] + S[j]) & 0xFF]
    return bytes(out)


# ── RSA (controller keypair; we hold the private key → derive public) ─────────
def _load_private_key():
    der = base64.b64decode(open(_SECRETS).read().strip())
    # recovered blob is PKCS#1 RSAPrivateKey; wrap as PEM for the loader
    from cryptography.hazmat.primitives.serialization import load_der_private_key
    try:
        return load_der_private_key(der, password=None)
    except Exception:
        pem = b"-----BEGIN RSA PRIVATE KEY-----\n" + base64.encodebytes(der) + b"-----END RSA PRIVATE KEY-----\n"
        return serialization.load_pem_private_key(pem, password=None)


_priv = None
def _priv_key():
    global _priv
    if _priv is None:
        _priv = _load_private_key()
    return _priv


def rsa_encrypt_pub(data: bytes) -> bytes:
    """Encrypt with the controller's PUBLIC key (PKCS1v15) — what the device does with the session key."""
    return _priv_key().public_key().encrypt(data, padding.PKCS1v15())


def rsa_decrypt_pub(ciphertext: bytes) -> bytes:
    """Reverse RsaCipher.encryptByPrivateKey: the controller RSA-encrypts with its PRIVATE key
    (raw RSA over PKCS#1 v1.5 type-01 padding); we recover with the PUBLIC key. Java's
    'RSA/ECB/PKCS1Padding' + private key = m^d mod n over a 00 01 FF..FF 00 <msg> block, so we do
    the public op c^e mod n and strip the type-01 pad. Used to read the account (username+MD5pass)
    out of the ADOPT_REQUEST the controller pushes to the device."""
    pub = _priv_key().public_key().public_numbers()
    n, e = pub.n, pub.e
    k = (n.bit_length() + 7) // 8
    c = int.from_bytes(ciphertext, "big")
    m = pow(c, e, n)
    eb = m.to_bytes(k, "big")
    # PKCS#1 v1.5 block for a private-key op: 0x00 0x01 <FF padding> 0x00 <data>
    if len(eb) < 11 or eb[0] != 0x00 or eb[1] != 0x01:
        raise ValueError("bad PKCS1 type-01 block")
    sep = eb.index(0x00, 2)
    return eb[sep + 1:]


def rsa_verify(data: bytes, sig: bytes) -> bool:
    """Verify a SHA1withRSA signature from the controller (its private key signs; we check w/ pub)."""
    try:
        _priv_key().public_key().verify(sig, data, padding.PKCS1v15(), hashes.SHA1())
        return True
    except Exception:
        return False


def _selftest() -> bool:
    ok = True
    v = rc4(b"Plaintext", b"Key").hex()
    print(f"{'OK ' if v=='bbf316e8d940af0ad3' else 'FAIL'} RC4('Key','Plaintext') = {v}")
    ok &= v == "bbf316e8d940af0ad3"
    try:
        k = _priv_key(); print(f"OK  RSA private key loaded ({k.key_size}-bit); public derivable")
        # round-trip: encrypt-with-public, decrypt-with-private
        ct = rsa_encrypt_pub(b"session-key-test")
        pt = k.decrypt(ct, padding.PKCS1v15())
        print(f"{'OK ' if pt==b'session-key-test' else 'FAIL'} RSA pub-encrypt / priv-decrypt round-trip")
        ok &= pt == b"session-key-test"
    except Exception as e:
        print(f"FAIL RSA: {e}"); ok = False
    print("ALL PASS" if ok else "FAILURES")
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if _selftest() else 1)
