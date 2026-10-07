import argparse
import struct

M64 = 0xFFFFFFFFFFFFFFFF
K0 = 0xC3A5C85C97CB3127
K1 = 0xB492B66FBE98F273
K2 = 0x9AE16A3B2F90404F
K3 = 0xC949D7C7509E6557
KMUL = 0x9DDFEA08EB382D69


def _f64(s, i):
    return struct.unpack_from("<Q", s, i)[0]


def _f32(s, i):
    return struct.unpack_from("<I", s, i)[0]


def _rot(v, shift):
    return v if shift == 0 else ((v >> shift) | (v << (64 - shift))) & M64


def _rot1(v, shift):
    return ((v >> shift) | (v << (64 - shift))) & M64


def _shift_mix(v):
    return v ^ (v >> 47)


def _hash16(u, v):
    a = ((u ^ v) * KMUL) & M64
    a ^= a >> 47
    b = ((v ^ a) * KMUL) & M64
    b ^= b >> 47
    return (b * KMUL) & M64


def _len0to16(s, n):
    if n > 8:
        a = _f64(s, 0)
        b = _f64(s, n - 8)
        return _hash16(a, _rot1((b + n) & M64, n)) ^ b
    if n >= 4:
        a = _f32(s, 0)
        return _hash16((n + (a << 3)) & M64, _f32(s, n - 4))
    if n > 0:
        a, b, c = s[0], s[n >> 1], s[n - 1]
        y = (a + (b << 8)) & 0xFFFFFFFF
        z = (n + (c << 2)) & 0xFFFFFFFF
        return (_shift_mix(((y * K2) ^ (z * K3)) & M64) * K2) & M64
    return K2


def _len17to32(s, n):
    a = (_f64(s, 0) * K1) & M64
    b = _f64(s, 8)
    c = (_f64(s, n - 8) * K2) & M64
    d = (_f64(s, n - 16) * K0) & M64
    return _hash16((_rot((a - b) & M64, 43) + _rot(c, 30) + d) & M64,
                   (a + _rot(b ^ K3, 20) - c + n) & M64)


def _weak32(w, x, y, z, a, b):
    a = (a + w) & M64
    b = _rot((b + a + z) & M64, 21)
    c = a
    a = (a + x + y) & M64
    b = (b + _rot(a, 44)) & M64
    return (a + z) & M64, (b + c) & M64


def _weak32s(s, i, a, b):
    return _weak32(_f64(s, i), _f64(s, i + 8), _f64(s, i + 16), _f64(s, i + 24), a, b)


def _len33to64(s, n):
    z = _f64(s, 24)
    a = (_f64(s, 0) + (n + _f64(s, n - 16)) * K0) & M64
    b = _rot((a + z) & M64, 52)
    c = _rot(a, 37)
    a = (a + _f64(s, 8)) & M64
    c = (c + _rot(a, 7)) & M64
    a = (a + _f64(s, 16)) & M64
    vf = (a + z) & M64
    vs = (b + _rot(a, 31) + c) & M64
    a = (_f64(s, 16) + _f64(s, n - 32)) & M64
    z = _f64(s, n - 8)
    b = _rot((a + z) & M64, 52)
    c = _rot(a, 37)
    a = (a + _f64(s, n - 24)) & M64
    c = (c + _rot(a, 7)) & M64
    a = (a + _f64(s, n - 16)) & M64
    wf = (a + z) & M64
    ws = (b + _rot(a, 31) + c) & M64
    r = _shift_mix(((vf + ws) * K2 + (wf + vs) * K0) & M64)
    return (_shift_mix((r * K0 + vs) & M64) * K2) & M64


def cityhash64(s: bytes) -> int:
    n = len(s)
    if n <= 32:
        return _len0to16(s, n) if n <= 16 else _len17to32(s, n)
    if n <= 64:
        return _len33to64(s, n)
    x = _f64(s, n - 40)
    y = (_f64(s, n - 16) + _f64(s, n - 56)) & M64
    z = _hash16((_f64(s, n - 48) + n) & M64, _f64(s, n - 24))
    v = _weak32s(s, n - 64, n, z)
    w = _weak32s(s, n - 32, (y + K1) & M64, x)
    x = (x * K1 + _f64(s, 0)) & M64
    left = (n - 1) & ~63
    i = 0
    while True:
        x = (_rot((x + y + v[0] + _f64(s, i + 8)) & M64, 37) * K1) & M64
        y = (_rot((y + v[1] + _f64(s, i + 48)) & M64, 42) * K1) & M64
        x ^= w[1]
        y = (y + v[0] + _f64(s, i + 40)) & M64
        z = (_rot((z + w[0]) & M64, 33) * K1) & M64
        v = _weak32s(s, i, (v[1] * K1) & M64, (x + w[0]) & M64)
        w = _weak32s(s, i + 32, (z + w[1]) & M64, (y + _f64(s, i + 16)) & M64)
        z, x = x, z
        i += 64
        left -= 64
        if left == 0:
            break
    return _hash16((_hash16(v[0], w[0]) + _shift_mix(y) * K1 + z) & M64,
                   (_hash16(v[1], w[1]) + x) & M64)


def cityhash64_seeds(s: bytes, seed0: int, seed1: int) -> int:
    return _hash16((cityhash64(s) - seed0) & M64, seed1)


def strcode64(text: str, terminator: bool = True) -> int:
    raw = text.encode("utf-8")
    seed1 = ((raw[0] << 16) + len(raw)) if raw else 0
    return cityhash64_seeds(raw + (b"\x00" if terminator else b""), K2, seed1) & 0xFFFFFFFFFFFF


def main():
    ap = argparse.ArgumentParser(description="Fox Engine string hashes (CityHash64 v1.0.3 based).")
    ap.add_argument("text", nargs="+")
    args = ap.parse_args()
    for t in args.text:
        print("%012X  %s" % (strcode64(t), t))


if __name__ == "__main__":
    main()
