import argparse
import hashlib
import struct
from pathlib import Path

import foxcrypt
import foxhash

HEADER = struct.Struct("<6sc3sI18xIII4x")
ENTRY = struct.Struct("<I4xi4xi4xi4x16s")
STRING = struct.Struct("<i4xi4x")


def decrypt_entry(data: bytes, name: str):
    if not data or data[0] not in (0x1B, 0x1C):
        return data
    key = bytearray(struct.pack("<Q", ~foxhash.strcode64(name.rsplit("/", 1)[-1].lower()) & 0xFFFFFFFFFFFFFFFF))
    out = bytearray(len(data) - 1)
    for i in range(len(data) - 1):
        key[i % 8] ^= data[i + 1]
        out[i] = key[i % 8]
    return bytes(out[:-1]) if out and out[-1] == 0 else data


def entry_data(info, entry):
    return decrypt_entry(info["data"][entry["offset"]:entry["offset"] + entry["size"]], entry["name"])


def read_fpk(data: bytes):
    if foxcrypt.is_encrypted(data):
        data = foxcrypt.decrypt(data)
    magic, kind, platform, size, version, file_count, ref_count = HEADER.unpack_from(data, 0)
    if magic != b"foxfpk":
        raise ValueError("not a Fox package")
    entries, refs = [], []
    pos = HEADER.size
    for _ in range(file_count):
        offset, length, name_off, name_len, md5 = ENTRY.unpack_from(data, pos)
        name = data[name_off:name_off + name_len].decode("utf-8", "replace")
        entries.append({"offset": offset, "size": length, "name": name, "md5": md5.hex(),
                        "name_ok": hashlib.md5(name.encode()).digest() == md5})
        pos += ENTRY.size
    for _ in range(ref_count):
        name_off, name_len = STRING.unpack_from(data, pos)
        refs.append(data[name_off:name_off + name_len].decode("utf-8", "replace"))
        pos += STRING.size
    info = {"data": data, "platform": platform.decode("ascii", "replace"), "kind": "fpkd" if kind == b"d" else "fpk", "size": size, "version": version}
    return info, entries, refs


def main():
    ap = argparse.ArgumentParser(description="List or extract a Fox Engine package (.fpk / .fpkd).")
    ap.add_argument("package", type=Path)
    ap.add_argument("--extract", type=Path, metavar="DIR")
    args = ap.parse_args()

    info, entries, refs = read_fpk(args.package.read_bytes())
    data = info["data"]
    print("%s %s version %d, %d files, %d references, size %d" % (info["kind"], info["platform"], info["version"], len(entries), len(refs), info["size"]))
    for e in entries:
        print("  %10d  %s%s" % (e["size"], e["name"], "" if e["name_ok"] else "  [name hash mismatch]"))
        if args.extract:
            target = args.extract / e["name"].split(":", 1)[-1].lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(entry_data(info, e))
    for r in refs:
        print("  ref %s" % r)


if __name__ == "__main__":
    main()
