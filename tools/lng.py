import argparse
import json
import struct
import sys


def parse(data):
    if data[:4] != b"LANG":
        raise ValueError("not a LANG file")
    version, endian, count, table, keys, values = struct.unpack_from("<I4sIIII", data, 4)
    if endian[:2] != b"LE":
        raise ValueError("only little endian LANG files are supported")
    entries = []
    for i in range(count):
        key_offset, value_offset = struct.unpack_from("<II", data, table + i * 8)
        key_at = keys + key_offset
        key = data[key_at:data.index(b"\0", key_at)].decode("ascii")
        value_at = values + value_offset
        color = struct.unpack_from("<H", data, value_at)[0]
        text = data[value_at + 2:data.index(b"\0", value_at + 2)].decode("utf-8")
        entries.append({"key": key, "color": color, "text": text})
    return {"version": version, "entries": entries}


def main():
    ap = argparse.ArgumentParser(description="Dump Fox Engine LANG v2 files (.lng, P.T.) as JSON.")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    out = {}
    for path in args.files:
        with open(path, "rb") as f:
            out[path] = parse(f.read())
    sys.stdout.reconfigure(encoding="utf-8")
    json.dump(out, sys.stdout, ensure_ascii=False, indent=1)
    print()


if __name__ == "__main__":
    main()
