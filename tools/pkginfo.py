import argparse
import struct
import sys
from pathlib import Path

PKG_MAGIC = 0x7F434E54
PFS_MAGIC = 20130315
ENTRY_ENCRYPTED = 0x80000000

SYSTEM_ENTRY_NAMES = {
    0x0001: "digests",
    0x0010: "entry_keys",
    0x0020: "image_key",
    0x0080: "general_digests",
    0x0100: "metas",
    0x0200: "entry_names",
    0x0400: "license.dat",
    0x0401: "license.info",
    0x0402: "nptitle.dat",
    0x0403: "npbind.dat",
    0x0404: "selfinfo.dat",
    0x0406: "imageinfo.dat",
    0x0407: "target-deltainfo.dat",
    0x0408: "origin-deltainfo.dat",
    0x0409: "psreserved.dat",
}

CONTENT_FLAGS = {
    0x00100000: "FIRST_PATCH",
    0x00200000: "PATCHGO",
    0x00400000: "REMASTER",
    0x00800000: "PS_CLOUD",
    0x02000000: "GD_AC",
    0x04000000: "NON_GAME",
    0x08000000: "UNKNOWN_0x8000000",
    0x40000000: "SUBSEQUENT_PATCH",
}

PFS_MODE_BITS = {0x1: "signed", 0x2: "64bit", 0x4: "encrypted", 0x8: "unknown_0x8"}


def cstring(buf, offset):
    end = buf.index(b"\x00", offset)
    return buf[offset:end].decode("ascii", "replace")


def read_header(f):
    f.seek(0)
    raw = f.read(0x1000)

    def u32(off):
        return struct.unpack_from(">I", raw, off)[0]

    def u64(off):
        return struct.unpack_from(">Q", raw, off)[0]

    if u32(0) != PKG_MAGIC:
        sys.exit("not a PS4 PKG")
    return {
        "pkg_type": u32(0x04),
        "file_count": u32(0x0C),
        "entry_count": u32(0x10),
        "entry_table_offset": u32(0x18),
        "body_offset": u64(0x20),
        "body_size": u64(0x28),
        "content_id": raw[0x40:0x64].decode("ascii"),
        "drm_type": u32(0x70),
        "content_type": u32(0x74),
        "content_flags": u32(0x78),
        "promote_size": u32(0x7C),
        "version_date": u32(0x80),
        "version_hash": u32(0x84),
        "pfs_image_count": u32(0x404),
        "pfs_image_flags": u64(0x408),
        "pfs_image_offset": u64(0x410),
        "pfs_image_size": u64(0x418),
        "mount_image_offset": u64(0x420),
        "mount_image_size": u64(0x428),
        "pkg_size": u64(0x430),
        "pfs_signed_size": u32(0x438),
        "pfs_cache_size": u32(0x43C),
    }


def read_entries(f, header):
    f.seek(header["entry_table_offset"])
    entries = []
    for _ in range(header["entry_count"]):
        eid, name_offset, flags1, flags2, offset, size = struct.unpack(">6I", f.read(32)[:24])
        entries.append(
            {"id": eid, "name_offset": name_offset, "flags1": flags1, "flags2": flags2, "offset": offset, "size": size}
        )
    names_entry = next((e for e in entries if e["id"] == 0x0200), None)
    names = b""
    if names_entry:
        f.seek(names_entry["offset"])
        names = f.read(names_entry["size"])
    for e in entries:
        if e["id"] in SYSTEM_ENTRY_NAMES:
            e["name"] = SYSTEM_ENTRY_NAMES[e["id"]]
        elif names and e["name_offset"]:
            e["name"] = cstring(names, e["name_offset"])
        else:
            e["name"] = "entry_%04X" % e["id"]
        e["encrypted"] = bool(e["flags1"] & ENTRY_ENCRYPTED)
    return entries


def read_pfs_header(f, offset):
    f.seek(offset)
    raw = f.read(0x400)
    version, magic, pfs_id = struct.unpack_from("<qqq", raw, 0)
    mode = struct.unpack_from("<H", raw, 0x1C)[0]
    block_size, backups = struct.unpack_from("<ii", raw, 0x20)
    blocks, dinodes, data_blocks, dinode_blocks, superroot = struct.unpack_from("<qqqqq", raw, 0x28)
    return {
        "version": version,
        "magic_ok": magic == PFS_MAGIC,
        "read_only": raw[0x1A],
        "mode": mode,
        "mode_flags": [name for bit, name in PFS_MODE_BITS.items() if mode & bit],
        "block_size": block_size,
        "blocks": blocks,
        "dinodes": dinodes,
        "data_blocks": data_blocks,
        "dinode_blocks": dinode_blocks,
        "superroot_inode": superroot,
    }


def parse_sfo(data):
    if data[:4] != b"\x00PSF":
        return {}
    _, key_table, data_table, count = struct.unpack_from("<4I", data, 4)
    params = {}
    for i in range(count):
        key_offset, fmt, length, _, data_offset = struct.unpack_from("<HHIII", data, 0x14 + i * 16)
        key = cstring(data, key_table + key_offset)
        value = data[data_table + data_offset : data_table + data_offset + length]
        if fmt == 0x0404:
            params[key] = struct.unpack_from("<I", value)[0]
        else:
            params[key] = value.rstrip(b"\x00").decode("utf-8", "replace")
    return params


def main():
    ap = argparse.ArgumentParser(description="Inspect a PS4 PKG and extract its plaintext sce_sys entries.")
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--extract", type=Path, metavar="DIR", help="write plaintext named entries into DIR")
    args = ap.parse_args()

    with args.pkg.open("rb") as f:
        header = read_header(f)
        entries = read_entries(f, header)
        pfs = read_pfs_header(f, header["pfs_image_offset"])

        flags = [name for bit, name in CONTENT_FLAGS.items() if header["content_flags"] & bit]
        print("content_id      %s" % header["content_id"])
        print("pkg_type        0x%08X" % header["pkg_type"])
        print("drm_type        0x%X" % header["drm_type"])
        print("content_type    0x%X" % header["content_type"])
        print("content_flags   0x%08X %s" % (header["content_flags"], " ".join(flags)))
        print("version_date    %08X" % header["version_date"])
        print("pkg_size        0x%X" % header["pkg_size"])
        print("pfs_image       offset 0x%X size 0x%X flags 0x%X" % (
            header["pfs_image_offset"], header["pfs_image_size"], header["pfs_image_flags"]))
        print("pfs_header      magic_ok %s mode 0x%X (%s) block_size 0x%X dinodes %d data_blocks 0x%X" % (
            pfs["magic_ok"], pfs["mode"], ", ".join(pfs["mode_flags"]), pfs["block_size"], pfs["dinodes"],
            pfs["data_blocks"]))
        print()
        print("  id    flags1    flags2    offset     size      name")
        for e in entries:
            print("  %04X  %08X  %08X  %9X  %8X  %s%s" % (
                e["id"], e["flags1"], e["flags2"], e["offset"], e["size"], e["name"],
                "  [encrypted]" if e["encrypted"] else ""))

        sfo_entry = next((e for e in entries if e["name"] == "param.sfo"), None)
        if sfo_entry:
            f.seek(sfo_entry["offset"])
            print()
            for key, value in parse_sfo(f.read(sfo_entry["size"])).items():
                print("  %-24s %s" % (key, "0x%X" % value if isinstance(value, int) else value))

        if args.extract:
            written = 0
            for e in entries:
                if e["id"] < 0x1000 or e["encrypted"]:
                    continue
                out = args.extract / e["name"]
                out.parent.mkdir(parents=True, exist_ok=True)
                f.seek(e["offset"])
                out.write_bytes(f.read(e["size"]))
                written += 1
            print("\nwrote %d plaintext entries to %s" % (written, args.extract))


if __name__ == "__main__":
    main()
