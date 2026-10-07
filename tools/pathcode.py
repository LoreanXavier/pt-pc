import argparse
import struct
from pathlib import Path

from foxhash import K1, K2, M64, _f64, _hash16, _len0to16, _len17to32, _len33to64, _rot, _shift_mix, _weak32s

ASSETS_PREFIX = "/Assets/"
APP_ASSETS_PREFIX = "/app0/as/"
KNOWN_ASSET_ROOTS = ("fox", "tpp", "sh", "mgo")
PATH_HASH_MASK = 0x3FFFFFFFFFFFF
META_FLAG = 1 << 50
TYPE_SHIFT = 51


def cityhash64(data: bytes) -> int:
    n = len(data)
    if n <= 32:
        return _len0to16(data, n) if n <= 16 else _len17to32(data, n)
    if n <= 64:
        return _len33to64(data, n)
    x = _f64(data, n - 40)
    y = (_f64(data, n - 16) + _f64(data, n - 56)) & M64
    z = _hash16((_f64(data, n - 48) + n) & M64, _f64(data, n - 24))
    v = _weak32s(data, n - 64, n, z)
    w = _weak32s(data, n - 32, (y + K1) & M64, x)
    x = (x * K1 + _f64(data, 0)) & M64
    remaining = (n - 1) & ~63
    pos = 0
    while True:
        x = (_rot((x + y + v[0] + _f64(data, pos + 8)) & M64, 37) * K1) & M64
        y = (_rot((y + v[1] + _f64(data, pos + 48)) & M64, 42) * K1) & M64
        x ^= w[1]
        y = (y + v[0] + _f64(data, pos + 40)) & M64
        z = (_rot((z + w[0]) & M64, 33) * K1) & M64
        v = _weak32s(data, pos, (v[1] * K1) & M64, (x + w[0]) & M64)
        w = _weak32s(data, pos + 32, (z + w[1]) & M64, (y + _f64(data, pos + 16)) & M64)
        z, x = x, z
        pos += 64
        remaining -= 64
        if remaining == 0:
            break
    return _hash16((_hash16(v[0], w[0]) + _shift_mix(y) * K1 + z) & M64,
                   (_hash16(v[1], w[1]) + x) & M64)


def seeded_hash(text: str) -> int:
    raw = text.encode("utf-8")
    seed1 = struct.unpack("<Q", raw[::-1][:8].ljust(8, b"\0"))[0]
    return _hash16((cityhash64(raw) - K2) & M64, seed1)


def extension_type(extension: str) -> int:
    extension = extension[1:] if extension.startswith(".") else extension
    return seeded_hash(extension) & 0x1FFF if extension else 0


def split_extension(path: str):
    name_start = path.rfind("/") + 1
    dot = path.find(".", name_start)
    return (path, "") if dot < 0 else (path[:dot], path[dot:])


def raw_path_code64(path: str) -> int:
    stem, extension = split_extension(path)
    if stem.startswith(ASSETS_PREFIX):
        stem = stem[len(ASSETS_PREFIX):]
    value = seeded_hash(stem) & PATH_HASH_MASK if stem else 0
    return (extension_type(extension) << TYPE_SHIFT) | value


def path_code64(path: str) -> int:
    if path.startswith(ASSETS_PREFIX):
        rest = path[len(ASSETS_PREFIX):]
        root = rest.split("/", 1)[0] if "/" in rest else None
        code = raw_path_code64(path)
        return code if root in KNOWN_ASSET_ROOTS else code | META_FLAG
    normalized = path.replace("\\", "/")
    if normalized.startswith("./"):
        normalized = normalized[2:]
    return raw_path_code64(normalized) | META_FLAG


def with_extension(code: int, extension: str) -> int:
    return (extension_type(extension) << TYPE_SHIFT) | (code & ((1 << TYPE_SHIFT) - 1))


def assets_path(app_path: str) -> str:
    return ASSETS_PREFIX + app_path[len(APP_ASSETS_PREFIX):] if app_path.startswith(APP_ASSETS_PREFIX) else app_path


def read_pathid_list(path: Path):
    data = Path(path).read_bytes()
    version, count, dir_count, name_count, flags = struct.unpack_from("<5I", data, 0)

    def align16(value):
        return (value + 15) & ~15

    codes = struct.unpack_from("<%dQ" % count, data, 0x20)
    record_offset = align16(0x20 + 8 * count)
    records = struct.unpack_from("<%dI" % count, data, record_offset)
    dir_table = align16(record_offset + 4 * count)
    dir_offsets = struct.unpack_from("<%dI" % dir_count, data, dir_table)
    name_table = align16(dir_table + 4 * dir_count)
    name_offsets = struct.unpack_from("<%dI" % name_count, data, name_table)
    strings = align16(name_table + 4 * name_count)

    def string_at(offset):
        start = strings + offset
        return data[start:data.index(b"\0", start)].decode("utf-8")

    dirs = [string_at(o) for o in dir_offsets]
    names = [string_at(o) for o in name_offsets]
    table = {}
    for code, record in zip(codes, records):
        table[code] = dirs[record >> 20] + "/" + names[record & 0xFFFF]
    return table


def main():
    ap = argparse.ArgumentParser(description="Fox PathCode64 hashes and the pathid_list_ps4.bin name table.")
    ap.add_argument("paths", nargs="*", help="paths to hash, for example /Assets/sh/foo/bar.ftex")
    ap.add_argument("--list", type=Path, metavar="PATHID_LIST", help="print every code and path of a pathid_list file")
    ap.add_argument("--verify", action="store_true", help="with --list: recompute every code from its path")
    args = ap.parse_args()

    for p in args.paths:
        print("%016X  %s" % (path_code64(p), p))
    if args.list:
        table = read_pathid_list(args.list)
        mismatches = 0
        for code, app_path in table.items():
            line = "%016X  %s" % (code, app_path)
            if args.verify:
                stem = app_path[len("/app0/"):]
                stem = stem[3:] if stem.startswith("as/") else stem
                if seeded_hash(stem) & PATH_HASH_MASK != code & PATH_HASH_MASK:
                    mismatches += 1
                    line += "  [hash mismatch]"
            print(line)
        if args.verify:
            print("%d entries, %d hash mismatches" % (len(table), mismatches))


if __name__ == "__main__":
    main()
