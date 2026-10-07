import argparse
import struct
from pathlib import Path

HEADER_SIZES = {0xA0F8EFE6: 8, 0xE3F8EFE6: 16}


def is_encrypted(data: bytes) -> bool:
    return len(data) >= 8 and struct.unpack_from("<I", data, 0)[0] in HEADER_SIZES


def decrypt(data: bytes) -> bytes:
    magic, key = struct.unpack_from("<II", data, 0)
    body = data[HEADER_SIZES[magic]:]
    step = (278 * key) & 0xFFFFFFFF
    block_key = (key | ((key ^ 25974) << 16)) & 0xFFFFFFFF
    out = bytearray(body)
    for i in range(0, len(body) - len(body) % 4, 4):
        word = struct.unpack_from("<I", body, i)[0]
        struct.pack_into("<I", out, i, word ^ block_key)
        block_key = (step + 48828125 * block_key) & 0xFFFFFFFF
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Decrypt Fox Engine files that start with 0xA0F8EFE6 or 0xE3F8EFE6.")
    ap.add_argument("source", type=Path, help="file or directory")
    ap.add_argument("target", type=Path, help="output file or directory")
    args = ap.parse_args()

    files = [args.source] if args.source.is_file() else sorted(p for p in args.source.rglob("*") if p.is_file())
    done = 0
    for src in files:
        data = src.read_bytes()
        if not is_encrypted(data):
            continue
        dst = args.target if args.source.is_file() else args.target / src.relative_to(args.source)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(decrypt(data))
        done += 1
    print("decrypted %d of %d files" % (done, len(files)))


if __name__ == "__main__":
    main()
