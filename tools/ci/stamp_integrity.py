"""Stamps the setup with the SHA-256 of itself (src/engine/platform/self_integrity.h, the check against a corrupt download):
finds the marker, hashes the file with the 32 digest bytes after it zeroed, and writes the digest there."""
import hashlib
import sys
from pathlib import Path

MARKER = bytes([0x7E, 0x50, 0x54, 0x2D, 0x49, 0x4E, 0x54, 0x45, 0x47, 0x52, 0x49, 0x54, 0x59, 0x2D, 0x76, 0x31])


def stamp(path: Path) -> str:
    data = bytearray(path.read_bytes())
    at = data.find(MARKER)
    if at < 0 or data.find(MARKER, at + 1) >= 0:
        raise SystemExit(f"{path}: integrity marker missing or not unique")
    data[at + 16:at + 48] = bytes(32)
    digest = hashlib.sha256(data).digest()
    data[at + 16:at + 48] = digest
    path.write_bytes(data)
    return digest.hex()


if __name__ == "__main__":
    for name in sys.argv[1:]:
        print(name, stamp(Path(name)))
