import argparse
import json
import struct
from pathlib import Path

MAGIC = b"SBPL"
HEADER = struct.Struct("<4sBHB")
ENTRY = struct.Struct("<4sII")
SAL_MAGIC = b"SAL3"
SAL_INDEX = struct.Struct("<QQ")
SAL_RECORD = struct.Struct("<I4sI")


def read_sbp(data: bytes):
    magic, count, header_size, _ = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError("not a Fox sound bank package")
    if header_size != HEADER.size + count * ENTRY.size:
        raise ValueError("unexpected header size %d for %d entries" % (header_size, count))
    entries = []
    for index in range(count):
        kind, offset, size = ENTRY.unpack_from(data, HEADER.size + index * ENTRY.size)
        kind = kind.rstrip(b"\0").decode("ascii")
        if offset + size > len(data):
            raise ValueError("entry %d runs past the end of the file" % index)
        entries.append({"kind": kind, "offset": offset, "size": size})
    return entries


def read_sal(data: bytes):
    if data[:4] != SAL_MAGIC:
        raise ValueError("not a SAL3 table")
    count = struct.unpack_from("<I", data, 4)[0]
    records = []
    for index in range(count):
        key, offset = SAL_INDEX.unpack_from(data, 8 + index * SAL_INDEX.size)
        action_count, action_kind, param_size = SAL_RECORD.unpack_from(data, offset)
        params = data[offset + SAL_RECORD.size:offset + SAL_RECORD.size + param_size]
        text_start = offset + 0x1E
        text_end = data.index(b"\0", text_start)
        trailer_start = (text_end + 4) & ~3
        records.append({
            "key": key,
            "key_hex": "%012X" % key,
            "offset": offset,
            "actions": action_count,
            "kind": action_kind.rstrip(b"\0").decode("ascii"),
            "params": params.hex(),
            "unknown": data[offset + SAL_RECORD.size + param_size:text_start].hex(),
            "subtitle_id": data[text_start:text_end].decode("ascii", "replace"),
            "key_repeat": int.from_bytes(data[trailer_start:trailer_start + 6], "big"),
        })
    return records


def describe(kind: str, blob: bytes) -> str:
    if kind == "bnk" and blob[:4] == b"BKHD":
        version, bank_id = struct.unpack_from("<II", blob, 8)
        return "Wwise bank version %d, id %08X" % (version, bank_id)
    if kind == "sab" and blob[:4] == SAL_MAGIC:
        return "SAL3 table, %d entries" % struct.unpack_from("<I", blob, 4)[0]
    return blob[:4].hex()


def main():
    ap = argparse.ArgumentParser(description="List or extract a Fox sound bank package (.sbp).")
    ap.add_argument("packages", type=Path, nargs="+")
    ap.add_argument("--extract", type=Path, metavar="DIR", help="write <package>.<kind> files into DIR")
    ap.add_argument("--sal", action="store_true", help="print the SAL3 records of .sab entries as JSON")
    args = ap.parse_args()

    for path in args.packages:
        data = path.read_bytes()
        entries = read_sbp(data)
        print("%s: %d entries, %d bytes" % (path.name, len(entries), len(data)))
        for entry in entries:
            blob = data[entry["offset"]:entry["offset"] + entry["size"]]
            print("  %-3s  offset 0x%08X  size %10d  %s" % (entry["kind"], entry["offset"], entry["size"], describe(entry["kind"], blob)))
            if args.sal and entry["kind"] == "sab":
                print(json.dumps(read_sal(blob), indent=1))
            if args.extract:
                args.extract.mkdir(parents=True, exist_ok=True)
                (args.extract / ("%s.%s" % (path.stem, entry["kind"]))).write_bytes(blob)


if __name__ == "__main__":
    main()
