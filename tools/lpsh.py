import argparse
import json
import struct
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FPK = REPO / "dump" / "fpk"
DEFAULT_OUT = REPO / "dump" / "lpsh"

HEADER = struct.Struct("<IIII")
COUNT_OFFSET = 0x7C
DIV_COUNT_OFFSET = 0x8C
DIV_TABLE_OFFSET = 0xB0
ENTRY = struct.Struct("<III")
COEFFICIENTS = 9
CHANNELS = 4
PROBE_BYTES = COEFFICIENTS * CHANNELS * 2
BASIS = ("L00", "L1-1", "L10", "L11", "L2-2", "L2-1", "L20", "L21", "L22")


def cstring(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("latin-1")


def read_lpsh(path):
    data = Path(path).read_bytes()
    version, header_size, file_size, type_hash = HEADER.unpack_from(data, 0)
    if file_size != len(data):
        raise ValueError(f"{path}: size field {file_size} != {len(data)}")
    count = struct.unpack_from("<I", data, COUNT_OFFSET)[0]
    divisions = struct.unpack_from("<I", data, DIV_COUNT_OFFSET)[0]
    div_table = list(struct.unpack_from(f"<{divisions}I", data, DIV_TABLE_OFFSET))
    table = DIV_TABLE_OFFSET + 4 * divisions
    probes = []
    for i in range(count):
        name_offset, data_offset, zero = ENTRY.unpack_from(data, table + ENTRY.size * i)
        values = np.frombuffer(data, dtype="<f2", count=COEFFICIENTS * CHANNELS, offset=data_offset)
        coefficients = values.astype(np.float32).reshape(COEFFICIENTS, CHANNELS)
        probes.append({"name": cstring(data, name_offset), "offset": data_offset,
                       "rgb": coefficients[:, :3].tolist(), "sky": coefficients[:, 3].tolist()})
    return {"version": version, "header_size": header_size, "type_hash": f"0x{type_hash:08x}",
            "divisions": div_table, "basis": BASIS, "probes": probes}


def evaluate(coefficients, n):
    x, y, z = n
    basis = np.array([0.2820948, 0.4886025 * y, 0.4886025 * z, 0.4886025 * x, 1.0925484 * x * y, 1.0925484 * y * z,
                      0.3153916 * (3 * z * z - 1), 1.0925484 * x * z, 0.5462742 * (x * x - y * y)])
    return basis @ np.asarray(coefficients)


def main():
    ap = argparse.ArgumentParser(description="Read P.T. light probe SH files (.lpsh).")
    ap.add_argument("inputs", nargs="*", type=Path, help=".lpsh files or folders (default: every .lpsh under dump/fpk)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    files = []
    for item in args.inputs or [DEFAULT_FPK]:
        files.extend(sorted(item.rglob("*.lpsh")) if item.is_dir() else [item])
    args.out.mkdir(parents=True, exist_ok=True)
    for path in files:
        parsed = read_lpsh(path)
        target = args.out / (path.stem + ".json")
        if target.exists() and json.loads(target.read_text())["probes"] != parsed["probes"]:
            target = args.out / (path.parent.name + "_" + path.stem + ".json")
        target.write_text(json.dumps(parsed, indent=1))
        print(f"{path.name}: {len(parsed['probes'])} probes, {len(parsed['divisions'])} divisions, "
              f"max L00 {max(max(p['rgb'][0]) for p in parsed['probes']):.3f} -> {target.name}")


if __name__ == "__main__":
    main()
