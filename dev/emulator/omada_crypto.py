"""omada_crypto.py — reproductions of the Omada controller's key-derivation, for the device emulator.

Interop with the user's OWN lab controller (SW 5.15.24.19). The controller derives its symmetric
keys via a deterministic Java "SHA1PRNG"-seeded KeyGenerator (see dev/findings-crypto.md); to speak
its protocol the emulated device must derive the identical bytes from the same seed string.

Self-test vectors below are ground truth captured from the real JVM (jshell, exact k() logic):
    SHA1PRNG-AES128("hello")        = 6b4f89a54e2d27ecd7e8da05b4ab8fd9
    SHA1PRNG-AES128("deviceKey123") = f7a433cafcd13c3292aabf03432ddce0
    SHA1PRNG-AES128("")             = be1bdec0aa74b4dcb079943e70528096
"""
import hashlib


def sha1prng_bytes(seed: bytes, n: int) -> bytes:
    """Sun/OpenJDK SHA1PRNG output stream for a fresh instance after a single setSeed(seed).

    state <- SHA1(seed); then per 20-byte block: out <- SHA1(state); state <- updateState(state, out).
    """
    state = hashlib.sha1(seed).digest()
    result = bytearray()
    while len(result) < n:
        out = hashlib.sha1(state).digest()
        result += out
        state = _update_state(state, out)
    return bytes(result[:n])


def _update_state(state: bytes, output: bytes) -> bytes:
    """The Sun SecureRandom state feedback: byte-wise add(state, output, +1 carry); if unchanged, bump[0]."""
    s = bytearray(state)
    last = 1
    changed = False
    for i in range(len(s)):
        v = (s[i] & 0xff) + (output[i] & 0xff) + last
        t = v & 0xff
        if s[i] != t:
            changed = True
        s[i] = t
        last = v >> 8
    if not changed:
        s[0] = (s[0] + 1) & 0xff
    return bytes(s)


def derive_aes_key(seed: str) -> bytes:
    """Omada k(str): AES-128 key = first 16 bytes of the SHA1PRNG stream seeded by the UTF-8 string."""
    return sha1prng_bytes(seed.encode("utf-8"), 16)


_VECTORS = {
    "hello":        "6b4f89a54e2d27ecd7e8da05b4ab8fd9",
    "deviceKey123": "f7a433cafcd13c3292aabf03432ddce0",
    "":             "be1bdec0aa74b4dcb079943e70528096",
}


def _selftest() -> bool:
    ok = True
    for seed, expect in _VECTORS.items():
        got = derive_aes_key(seed).hex()
        status = "OK " if got == expect else "FAIL"
        if got != expect:
            ok = False
        print(f"{status} k({seed!r:14}) = {got}  expect {expect}")
    print("ALL VECTORS PASS" if ok else "MISMATCH — algorithm needs a fix")
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if _selftest() else 1)
