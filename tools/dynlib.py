import argparse
import base64
import hashlib
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000

DT_NAMES = {
    0x01: "NEEDED", 0x0C: "INIT", 0x0D: "FINI", 0x0E: "SONAME", 0x19: "INIT_ARRAY", 0x1A: "FINI_ARRAY",
    0x1B: "INIT_ARRAYSZ", 0x1C: "FINI_ARRAYSZ", 0x1E: "FLAGS", 0x20: "PREINIT_ARRAY", 0x21: "PREINIT_ARRAYSZ",
    0x61000005: "SCE_IDTABENTSZ", 0x61000007: "SCE_FINGERPRINT", 0x61000009: "SCE_ORIGINAL_FILENAME",
    0x6100000D: "SCE_MODULE_INFO", 0x6100000F: "SCE_NEEDED_MODULE", 0x61000011: "SCE_MODULE_ATTR",
    0x61000013: "SCE_EXPORT_LIB", 0x61000015: "SCE_IMPORT_LIB", 0x61000017: "SCE_EXPORT_LIB_ATTR",
    0x61000019: "SCE_IMPORT_LIB_ATTR", 0x61000025: "SCE_HASH", 0x61000027: "SCE_PLTGOT",
    0x61000029: "SCE_JMPREL", 0x6100002B: "SCE_PLTREL", 0x6100002D: "SCE_PLTRELSZ", 0x6100002F: "SCE_RELA",
    0x61000031: "SCE_RELASZ", 0x61000033: "SCE_RELAENT", 0x61000035: "SCE_STRTAB", 0x61000037: "SCE_STRSZ",
    0x61000039: "SCE_SYMTAB", 0x6100003B: "SCE_SYMENT", 0x6100003D: "SCE_HASHSZ", 0x6100003F: "SCE_SYMTABSZ",
    0x6FFFFFF9: "RELACOUNT", 0x6FFFFFFB: "FLAGS_1",
}

RELOC_NAMES = {1: "R_X86_64_64", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE", 16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64"}
SYM_TYPES = {0: "notype", 1: "object", 2: "func", 3: "section", 4: "file", 6: "tls"}
SYM_BINDS = {0: "local", 1: "global", 2: "weak"}

ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def name_to_nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(digest[:8][::-1], b"+-").rstrip(b"=").decode()


def decode_id(text):
    value = 0
    for ch in text:
        value = value * 64 + ID_ALPHABET.index(ch)
    return value


def load_nid_db(paths):
    names = {}
    for path in paths:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        for nid, sym in re.findall(r'obf="([^"]+)"\s+sym="([^"]*)"', text):
            names.setdefault(nid, sym)
        for nid, sym in re.findall(r'LIB_FUNCTION\(\s*"([A-Za-z0-9+\-]{11})"\s*,[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,\s*([A-Za-z_][\w:]*)\s*\)', text):
            names.setdefault(nid, sym.split("::")[-1])
    return names


def cstring(buf, offset):
    end = buf.index(b"\x00", offset)
    return buf[offset:end].decode("ascii", "replace")


def parse(elf):
    e_phoff, e_phentsize, e_phnum = struct.unpack_from("<Q", elf, 0x20)[0], *struct.unpack_from("<HH", elf, 0x36)
    phdrs = [struct.unpack_from("<IIQQQQQQ", elf, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    dyn = next(p for p in phdrs if p[0] == PT_DYNAMIC)
    dyndata = next(p for p in phdrs if p[0] == PT_SCE_DYNLIBDATA)
    base = dyndata[2]

    tags = []
    for i in range(dyn[5] // 16):
        tag, value = struct.unpack_from("<qQ", elf, dyn[2] + i * 16)
        if tag == 0:
            break
        tags.append((tag, value))
    first = {}
    for tag, value in tags:
        first.setdefault(tag, value)

    strtab = elf[base + first[0x61000035]: base + first[0x61000035] + first[0x61000037]]
    modules, libraries = {}, {}
    info = {"needed": [], "fingerprint": None, "original_filename": None, "tags": Counter()}
    for tag, value in tags:
        info["tags"][DT_NAMES.get(tag, hex(tag))] += 1
        if tag in (0x6100000D, 0x6100000F):
            modules[value >> 48] = {"name": cstring(strtab, value & 0xFFFFFFFF), "major": (value >> 40) & 0xFF,
                                    "minor": (value >> 32) & 0xFF, "self": tag == 0x6100000D}
        elif tag in (0x61000013, 0x61000015):
            libraries[value >> 48] = {"name": cstring(strtab, value & 0xFFFFFFFF), "version": (value >> 32) & 0xFFFF,
                                      "export": tag == 0x61000013}
        elif tag == 0x01:
            info["needed"].append(cstring(strtab, value))
        elif tag == 0x61000007:
            info["fingerprint"] = elf[base + value: base + value + 20].hex()
        elif tag == 0x61000009:
            info["original_filename"] = cstring(strtab, value)
    for key in (0x0C, 0x0D, 0x19, 0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x61000027):
        if key in first:
            info[DT_NAMES[key]] = first[key]

    symtab_off, symtab_size = base + first[0x61000039], first[0x6100003F]
    symbols = []
    for i in range(symtab_size // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", elf, symtab_off + i * 24)
        symbols.append({"index": i, "raw": cstring(strtab, st_name), "type": SYM_TYPES.get(st_info & 0xF, st_info & 0xF),
                        "bind": SYM_BINDS.get(st_info >> 4, st_info >> 4), "shndx": st_shndx, "value": st_value, "size": st_size})
    for s in symbols:
        parts = s["raw"].split("#")
        s["nid"] = parts[0]
        s["lib"] = libraries.get(decode_id(parts[1]), {}).get("name") if len(parts) == 3 else None
        s["module"] = modules.get(decode_id(parts[2]), {}).get("name") if len(parts) == 3 else None

    def relocs(off_tag, size_tag):
        out = []
        if off_tag in first:
            start, size = base + first[off_tag], first.get(size_tag, 0)
            for i in range(size // 24):
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", elf, start + i * 24)
                out.append((r_offset, r_info >> 32, r_info & 0xFFFFFFFF, r_addend))
        return out

    return info, modules, libraries, symbols, relocs(0x61000029, 0x6100002D), relocs(0x6100002F, 0x61000031)


def main():
    ap = argparse.ArgumentParser(description="Dump the SCE dynamic linking data of a plain PS4 ELF (see self2elf.py).")
    ap.add_argument("elf", type=Path)
    ap.add_argument("--nid-db", action="append", default=[], help="GhidraOrbis nid_db.xml or shadPS4 sources; repeatable")
    ap.add_argument("--markdown", type=Path, help="write the import table as markdown")
    args = ap.parse_args()

    names = load_nid_db(args.nid_db)
    info, modules, libraries, symbols, jmprel, rela = parse(args.elf.read_bytes())
    got_of = {sym: off for off, sym, rtype, _ in jmprel if rtype == 7}
    data_refs = Counter(sym for _, sym, rtype, _ in rela if rtype in (1, 6))

    imports = [s for s in symbols if s["shndx"] == 0 and s["lib"]]
    exports = [s for s in symbols if s["shndx"] != 0 and s["lib"]]
    for s in imports + exports:
        s["name"] = names.get(s["nid"])
        s["got"] = got_of.get(s["index"])
        s["data_refs"] = data_refs.get(s["index"], 0)

    print("original_filename %s  fingerprint %s" % (info["original_filename"], info["fingerprint"]))
    print("init 0x%X fini 0x%X pltgot 0x%X  init_array 0x%X (%d)  fini_array 0x%X (%d)  preinit_array %s" % (
        info.get("INIT", 0), info.get("FINI", 0), info.get("SCE_PLTGOT", 0), info.get("INIT_ARRAY", 0),
        info.get("INIT_ARRAYSZ", 0) // 8, info.get("FINI_ARRAY", 0), info.get("FINI_ARRAYSZ", 0) // 8,
        "0x%X (%d)" % (info["PREINIT_ARRAY"], info["PREINIT_ARRAYSZ"] // 8) if "PREINIT_ARRAY" in info else "none"))
    print("modules:", ", ".join("%s%s %d.%d" % ("*" if m["self"] else "", m["name"], m["major"], m["minor"]) for _, m in sorted(modules.items())))
    print("libraries: %d import, %d export" % (sum(not l["export"] for l in libraries.values()), sum(l["export"] for l in libraries.values())))
    print("symbols %d, imports %d (resolved %d), exports %d" % (len(symbols), len(imports), sum(1 for s in imports if s["name"]), len(exports)))
    print("jmprel %d  rela %d  rela types %s" % (len(jmprel), len(rela), dict(Counter(RELOC_NAMES.get(t, t) for _, _, t, _ in rela))))

    by_lib = defaultdict(list)
    for s in imports:
        by_lib[(s["module"], s["lib"])].append(s)
    for (module, lib), syms in sorted(by_lib.items(), key=lambda kv: (kv[0][0] or "", kv[0][1] or "")):
        print("\n[%s / %s] %d" % (module, lib, len(syms)))
        for s in sorted(syms, key=lambda s: s["name"] or "~" + s["nid"]):
            print("  %-11s %-6s %-40s%s" % (s["nid"], s["type"], s["name"] or "?", "  data_refs=%d" % s["data_refs"] if s["data_refs"] else ""))
    if exports:
        print("\n[exports]")
        for s in exports:
            print("  %-11s %-6s 0x%010X %-8d %s (%s)" % (s["nid"], s["type"], s["value"], s["size"], s["name"] or "?", s["lib"]))

    if args.markdown:
        lines = ["# Imports", "", "Generated by `tools/dynlib.py` from `%s`." % args.elf.name, "",
                 "Modules: " + ", ".join("%s %d.%d" % (m["name"], m["major"], m["minor"]) for _, m in sorted(modules.items()) if not m["self"]), ""]
        lines += ["| module | library | imports | resolved |", "| --- | --- | --- | --- |"]
        for (module, lib), syms in sorted(by_lib.items(), key=lambda kv: (kv[0][0] or "", kv[0][1] or "")):
            lines.append("| %s | %s | %d | %d |" % (module, lib, len(syms), sum(1 for s in syms if s["name"])))
        for (module, lib), syms in sorted(by_lib.items(), key=lambda kv: (kv[0][0] or "", kv[0][1] or "")):
            lines += ["", "## %s / %s" % (module, lib), "", "| NID | type | name | GOT |", "| --- | --- | --- | --- |"]
            for s in sorted(syms, key=lambda s: s["name"] or "~" + s["nid"]):
                lines.append("| `%s` | %s | %s | %s |" % (s["nid"], s["type"], "`%s`" % s["name"] if s["name"] else "unresolved",
                                                          "0x%X" % s["got"] if s["got"] else ""))
        args.markdown.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
