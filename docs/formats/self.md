# SELF (eboot.bin, sce_module/*.prx)

All three executables in CUSA01127 are fake-signed SELF files. `tools/self2elf.py` turns them into plain ELF files in `dump/elf/`. Field names follow shadPS4 (`src/core/loader/elf.h`).

## Header (0x20 bytes, little-endian)

| offset | field | eboot.bin |
| --- | --- | --- |
| 0x00 | magic | 0x1D3D154F (bytes `4F 15 3D 1D`) |
| 0x04 | version, mode, endian, attributes | 00 01 01 12 |
| 0x08 | category, program_type | 1, 0x1 |
| 0x0C | header_size | 0x400 |
| 0x0E | meta_size | 0x3D0 |
| 0x10 | file_size | 0x1961BD0 |
| 0x18 | segment_count | 8 |
| 0x1A | unknown, always 0x22 in shadPS4 | 0x22 |

## Segment table

Right after the header, 0x20 bytes per entry: flags, file offset, file size, memory size (u64 each). Flag bits as named by shadPS4: 0x1 ordered, 0x2 encrypted, 0x4 signed, 0x8 compressed, 0x800 blocked. Bits 20..31 hold an id. For blocked entries the id is the index of the ELF program header whose data the entry carries. The non-blocked entries precede a blocked one and carry its block digests.

Every entry in all three files has flags `0x..0004` (signed) or `0x..2804` (signed, blocked). None is encrypted or compressed, so the segment data is plaintext.

## Embedded ELF

The ELF header follows the segment table, the program headers follow at `e_phoff` (0x40) relative to it. There are no section headers. The extended info block (program authority id `0x3100000000000002`, program type, app version, firmware version) sits after the program headers, aligned to 16.

`PT_SCE_VERSION` has no data in any of the three SELF files, so `self2elf.py` zero-fills it. Every other program header either has its own blocked segment or lies inside one.

## eboot.elf layout

`e_type` 0xFE00 (ET_SCE_EXEC, fixed address), entry 0x4004F0, interpreter `/libexec/ld-elf.so.1`.

| # | type | vaddr | file size | mem size |
| --- | --- | --- | --- | --- |
| 0 | LOAD r-x | 0x400000 | 0x177C6E0 | 0x177C6E0 |
| 1 | LOAD rw- | 0x1B80000 | 0x95E28 | 0x32B0C0 |
| 2 | SCE_PROCPARAM | 0x1B80000 | 0x40 | 0x40 |
| 3 | DYNAMIC | not mapped | 0x940 | |
| 4 | INTERP | 0x400000 | 0x15 | |
| 5 | TLS | 0x1B80040 | 0x4 | 0x78 |
| 6 | GNU_EH_FRAME | 0x19CD6E4 | 0x1AEFFC | |
| 7 | SCE_DYNLIBDATA | not mapped | 0x142390 | |
| 8 | SCE_COMMENT | not mapped | 0x60 | |
| 9 | SCE_VERSION | not mapped | 0xA73 (absent) | |

`SCE_PROCPARAM` starts with size 0x40, magic `ORBI`, version 1, SDK version 0x01600051. `SCE_COMMENT` holds the original link path `C:/develop/pt14_master/git_clone/_intermediate/Sh_game_ps4/Release/Sh_main_ps4.elf`.

The bundled `libc.prx` and `libSceFios2.prx` are ET_SCE_DYNLIB (0xFE18) and come from Sony's own build tree (`W:/Build/J00580091/...`).
