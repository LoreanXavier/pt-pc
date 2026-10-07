import argparse
import struct
import sys
import zlib
from pathlib import Path

SELF_MAGIC = 0x1D3D154F
SELF_HEADER = struct.Struct("<IBBBBBBHHHIIHH4x")
SELF_SEGMENT = struct.Struct("<QQQQ")
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
ELF_PHDR = struct.Struct("<IIQQQQQQ")

SEG_ENCRYPTED = 0x2
SEG_COMPRESSED = 0x8
SEG_BLOCKED = 0x800

PT_NAMES = {
    1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
    0x61000000: "SCE_DYNLIBDATA", 0x61000001: "SCE_PROCPARAM", 0x61000002: "SCE_MODULE_PARAM",
    0x61000010: "SCE_RELRO", 0x6474E550: "GNU_EH_FRAME", 0x6FFFFF00: "SCE_COMMENT", 0x6FFFFF01: "SCE_VERSION",
}


def convert(src: bytes):
    magic, _, _, _, _, _, _, _, _, _, _, _, seg_count, _ = SELF_HEADER.unpack_from(src, 0)
    if magic != SELF_MAGIC:
        raise ValueError("not a SELF file")
    segments = [SELF_SEGMENT.unpack_from(src, SELF_HEADER.size + i * SELF_SEGMENT.size) for i in range(seg_count)]

    elf_at = SELF_HEADER.size + seg_count * SELF_SEGMENT.size
    ehdr = list(ELF_HEADER.unpack_from(src, elf_at))
    e_phoff, e_phentsize, e_phnum = ehdr[5], ehdr[9], ehdr[10]
    phdrs = [ELF_PHDR.unpack_from(src, elf_at + e_phoff + i * e_phentsize) for i in range(e_phnum)]

    end = max([e_phoff + e_phnum * e_phentsize] + [p[2] + p[5] for p in phdrs])
    out = bytearray(end)

    ehdr[6], ehdr[11], ehdr[12], ehdr[13] = 0, 0, 0, 0
    ELF_HEADER.pack_into(out, 0, *ehdr)
    for i, p in enumerate(phdrs):
        ELF_PHDR.pack_into(out, e_phoff + i * e_phentsize, *p)

    filled = set()
    for flags, offset, file_size, mem_size in segments:
        if not flags & SEG_BLOCKED:
            continue
        if flags & SEG_ENCRYPTED:
            raise ValueError("segment %d is encrypted, only fake-signed SELF files are supported" % ((flags >> 20) & 0xFFF))
        phdr_id = (flags >> 20) & 0xFFF
        data = src[offset:offset + file_size]
        if flags & SEG_COMPRESSED:
            data = zlib.decompress(data)
        p_offset, p_filesz = phdrs[phdr_id][2], phdrs[phdr_id][5]
        out[p_offset:p_offset + min(len(data), p_filesz)] = data[:p_filesz]
        filled.add(phdr_id)

    covered = []
    for i, p in enumerate(phdrs):
        if i in filled or p[5] == 0:
            continue
        inside = any(phdrs[j][2] <= p[2] and p[2] + p[5] <= phdrs[j][2] + phdrs[j][5] for j in filled)
        covered.append((i, PT_NAMES.get(p[0], hex(p[0])), inside))
    return bytes(out), phdrs, filled, covered


def main():
    ap = argparse.ArgumentParser(description="Convert a fake-signed PS4 SELF (eboot.bin, .prx, .sprx) into a plain ELF.")
    ap.add_argument("self_file", type=Path)
    ap.add_argument("elf_file", type=Path)
    args = ap.parse_args()

    try:
        elf, phdrs, filled, covered = convert(args.self_file.read_bytes())
    except ValueError as e:
        sys.exit("%s: %s" % (args.self_file, e))
    args.elf_file.parent.mkdir(parents=True, exist_ok=True)
    args.elf_file.write_bytes(elf)

    print("%s -> %s (%d bytes)" % (args.self_file, args.elf_file, len(elf)))
    for i, p in enumerate(phdrs):
        state = "data" if i in filled else ""
        for j, name, inside in covered:
            if j == i:
                state = "inside another segment" if inside else "no data in SELF, zero-filled"
        print("  ph %2d %-16s off 0x%08X vaddr 0x%010X filesz 0x%08X memsz 0x%08X  %s" % (
            i, PT_NAMES.get(p[0], hex(p[0])), p[2], p[3], p[5], p[6], state))


if __name__ == "__main__":
    main()
