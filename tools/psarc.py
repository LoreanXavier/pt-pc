import argparse
import struct
import zlib
from pathlib import Path

HEADER = struct.Struct(">4sHH4sIIIII")


def read_toc(f):
    f.seek(0)
    magic, major, minor, compression, toc_length, entry_size, entry_count, block_size, flags = HEADER.unpack(f.read(HEADER.size))
    if magic != b"PSAR":
        raise ValueError("not a PSARC file")
    toc = f.read(toc_length - HEADER.size)
    entries = []
    for i in range(entry_count):
        raw = toc[i * entry_size:(i + 1) * entry_size]
        entries.append({
            "md5": raw[:16].hex(),
            "block": struct.unpack(">I", raw[16:20])[0],
            "size": int.from_bytes(raw[20:25], "big"),
            "offset": int.from_bytes(raw[25:30], "big"),
        })
    width = 2
    while (1 << (8 * width)) < block_size:
        width += 1
    table = toc[entry_count * entry_size:]
    block_sizes = [int.from_bytes(table[i:i + width], "big") for i in range(0, len(table) - width + 1, width)]
    info = {"version": "%d.%d" % (major, minor), "compression": compression.decode(), "block_size": block_size,
            "flags": flags, "entries": entry_count}
    return info, entries, block_sizes


def read_entry(f, entry, block_sizes, block_size):
    out = bytearray()
    f.seek(entry["offset"])
    index = entry["block"]
    while len(out) < entry["size"]:
        stored = block_sizes[index] or block_size
        chunk = f.read(stored)
        remaining = entry["size"] - len(out)
        if stored == block_size and remaining >= block_size:
            out += chunk
        else:
            try:
                out += zlib.decompress(chunk)
            except zlib.error:
                out += chunk
        index += 1
    return bytes(out[:entry["size"]])


def main():
    ap = argparse.ArgumentParser(description="List or extract a PlayStation PSARC archive.")
    ap.add_argument("psarc", type=Path)
    ap.add_argument("--extract", type=Path, metavar="DIR")
    args = ap.parse_args()

    with args.psarc.open("rb") as f:
        info, entries, block_sizes = read_toc(f)
        manifest = read_entry(f, entries[0], block_sizes, info["block_size"]).decode("utf-8", "replace")
        names = ["<manifest>"] + manifest.replace("\r", "").split("\n")
        print("PSARC %(version)s %(compression)s block 0x%(block_size)X flags %(flags)d entries %(entries)d" % info)
        for i, e in enumerate(entries):
            name = names[i] if i < len(names) else "<unnamed %d>" % i
            print("%4d %10d  0x%010X  %s" % (i, e["size"], e["offset"], name))
            if args.extract and i > 0:
                target = args.extract / name.lstrip("/")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(read_entry(f, e, block_sizes, info["block_size"]))


if __name__ == "__main__":
    main()
