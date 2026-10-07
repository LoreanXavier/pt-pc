# Textures: FTEX, FTEXS, PFTXS, texture.qar, pathid_list_ps4.bin

Tools: `tools/ftex.py` (convert to PNG), `tools/qar.py` (list or extract `texture.qar`), `tools/pathcode.py` (PathCode64
and the pathid list).

## Where the pixel data lives

A texture is one FTEX header (`.ftex`) plus one to six FTEXS files (`.1.ftexs` .. `.6.ftexs`) that hold the mips.

- `texture.qar` holds the complete set, header and every FTEXS file, for 1,061 textures.
- Each `.pftxs` pack (one per level, plus resident, player parts and UI) holds copies of the FTEX headers of its
  textures and of the low resolution FTEXS files. The two highest resolution files (mips 0 and 1; only mip 0 for 2-file
  textures) are only in `texture.qar`. Every copy in a pack is byte-identical to the one in `texture.qar`.
- 4 textures are not in `texture.qar`: `cm_flat_white` (complete in `ui_default_data.pftxs`) and the three
  `fox_pri_soil_*` maps in `ui_default_lang.pftxs`, whose mips 0 and 1 exist nowhere in the game files.
- 57 textures are only in `texture.qar` (effects, noise, load icon).
- The 27 loose `.ftex` files inside the fpk packages are the same headers again; their data is in `texture.qar`.

Pixel data is stored untiled (see Tiling).

## PathCode64

`texture.qar` and `pathid_list_ps4.bin` key files by a 64-bit path code:

```
stem, ext   = path split at the first '.' after the last '/'
text        = stem without a leading "/Assets/"
seed1       = little-endian u64 of the last 8 bytes of text, reversed (last byte lowest)
hash(s)     = HashLen16(CityHash64(s) - 0x9AE16A3B2F90404F, seed1(s))
type        = hash(ext without '.') & 0x1FFF
code        = type << 51 | meta << 50 | hash(text) & 0x3FFFFFFFFFFFF
meta        = 0 for "/Assets/<root>/..." with root in {fox, tpp, sh, mgo}, else 1
```

This is the same scheme as GzsTool's `HashFileNameWithExtension` (MGSV). All 4,955 codes of `pathid_list_ps4.bin`
recompute from their paths (`pathcode.py --list <file> --verify`), with `/app0/as/` read as `/Assets/`. CityHash64 is
v1.0.3; `pathcode.py` carries its long-input loop (`fox2.md`, StrCode64).

Extension types seen:

| type | ext | | type | ext |
| --- | --- | --- | --- | --- |
| 685 | ftex | | 164 | fmtt |
| 5720 | 1.ftexs | | 796 | lua |
| 5806 | 2.ftexs | | 1439 | fsop |
| 2787 | 3.ftexs | | 1677 | vrc |
| 2038 | 4.ftexs | | 1740 | ffnt |
| 4971 | 5.ftexs | | 1752 | bnk |
| 2084 | 6.ftexs | | 1977 | tga |
| 5727 | pftxs | | 2629 / 7594 | fpk / fpkd |
| 3131 | fsm | | 3243 / 5980 | gnd / sbp |

## pathid_list_ps4.bin

Maps every shipped file's PathCode64 back to its path string. Little-endian, every table starts 16-aligned.

| offset | type | field | value |
| --- | --- | --- | --- |
| 0x00 | u32 | version (guess) | 1 |
| 0x04 | u32 | code count n | 4955 |
| 0x08 | u32 | directory count | 135 |
| 0x0C | u32 | name count | 1096 |
| 0x10 | u32 | unknown | 1 |
| 0x20 | u64[n] | PathCode64, sorted ascending | |
| align16 | u32[n] | record: bits 0..15 name index, 16..19 zero, 20..31 directory index | |
| align16 | u32[dirs] | directory string offsets | |
| align16 | u32[names] | name string offsets | |
| align16 | char[] | NUL-terminated strings; offsets are relative to this point | directories like `/app0/as/sh/chara/bab/Pictures`, names without extension |

The list covers texture.qar (4,862 codes) and the chunk1.psarc files (93 codes).

## texture.qar

A Fox QAR archive without a header: the footer is read from the end, then the entry table; entries are found by binary
search on the code. Data offset = `offset16 << 4`.

Footer, last 0x24 bytes:

| offset | type | field | value |
| --- | --- | --- | --- |
| 0x00 | u64 | unknown | 0x352E1438 |
| 0x08 | u64 | unknown | 0 |
| 0x10 | u32 | entry count | 4862 |
| 0x14 | u16 | flags: bit 0 = NUL-terminated names follow the entry table | 0 |
| 0x16 | u16 | magic | 0x7161 |
| 0x18 | u32 | entry table offset >> 4 | 0x0352E17E |
| 0x1C | u32 | unknown | 0 |
| 0x20 | u32 | unknown | 0x14 |

Entry, 16 bytes: u64 PathCode64, u32 data offset >> 4, u32 size. In the file the entries are in data order: files
sorted by path, and per texture `.1.ftexs`, `.2.ftexs`, ..., then `.ftex`. Every file starts 16-aligned; the data area
is contiguous from 0 to the table.

## PFTXS

One pack per level.

| offset | type | field |
| --- | --- | --- |
| 0x00 | char[4] | `PFTX` |
| 0x04 | f32 | 1.0 |
| 0x08 | u32 | file size |
| 0x0C | u32 | texture count |
| 0x10 | u32 | offset of the first texture record |
| 0x14 | {u32 name offset, u32 FTEX header size}[count] | |
| | strings | full path of a texture, or `@name` = same directory as the previous full path; no extension |

Texture record, repeated, each 16-aligned:

| part | layout |
| --- | --- |
| FTEX header | identical to the `.ftex` in texture.qar |
| PSUB | `PSUB`, u32 k, k x {u32 absolute offset, u32 size} for `.1.ftexs` .. `.k.ftexs`, padded with 0xCC to 16 |
| data | the k FTEXS files, each 16-aligned |

After the last record: `EOPF`, then 0xCC padding to a multiple of 0x800. A pack always holds mip 2 and below (or
everything below mip 0 for textures with two FTEXS files, everything for one).

## FTEX (version 2.03)

| offset | type | field | values in P.T. |
| --- | --- | --- | --- |
| 0x00 | char[4] | `FTEX` (`XETF` = big-endian) | |
| 0x04 | f32 | version | 2.03 |
| 0x08 | u16 | pixel format, see below | 0, 2, 4 |
| 0x0A | u16 | width | 16..4096, all powers of two except 256x2 |
| 0x0C | u16 | height | |
| 0x0E | u16 | depth | 1, 16 (two 16x16x16 LUTs) |
| 0x10 | u8 | mip count | |
| 0x11 | u8 | filter (0 point, 1 linear with point mip, 2 trilinear) | 2, 0 once |
| 0x12 | u16 | address mode, one nibble per axis (0 clamp, 1 wrap, 2 mirror, 3 border) | 0x11, 0x0, 0x111 |
| 0x14 | u16 | unknown | 1 |
| 0x16 | u16 | unknown | 0 |
| 0x18 | u32 | unknown | 0 |
| 0x1C | u32 | flags | see below |
| 0x20 | u8 | FTEXS file count | 1..6 |
| 0x21 | u8 | first mip stored in `.1.ftexs` | = count - 1 |
| 0x22 | 14 bytes | zero | |
| 0x30 | 16 bytes | unknown hash, unique per texture content | |
| 0x40 | 16 bytes x mips x faces | mip entries, face-major (face 0 mips 0..n-1, then face 1 ...) | |

Flags: bit 0 = mips stored as zlib chunks, bit 1 = sRGB (set on all `_bsm` colour maps and on no `_nrm`, `_srm`, `_trm`,
`_mtm` map), bit 2 = cube map, 6 faces, bit 3 = normal map, bit 24 = mips in external `.N.ftexs` files. Seen: 0x1000001,
0x1000003, 0x1000009, 0x1000007 (8 cube maps), 0x1000000, 0x1000008, 0x1000002.

Mip entry:

| offset | type | field |
| --- | --- | --- |
| 0x0 | u32 | offset in the FTEXS file (or after the header in the .ftex when the file number is 0) |
| 0x4 | u32 | unpacked size |
| 0x8 | u32 | stored size: 0 when not chunked, else chunk table + chunk data padded to 16 |
| 0xC | u8 | mip level |
| 0xD | u8 | FTEXS file number, 0 = inside the .ftex |
| 0xE | u16 | chunk count |

File assignment: starting from the smallest mip, mips go into `.1.ftexs` while their running total is at most 0x8000
bytes; each larger mip gets its own file, so mip 0 is in the highest-numbered file.

Pixel formats:

| value | format | textures in texture.qar |
| --- | --- | --- |
| 0 | B8G8R8A8 (bytes are B, G, R, A) | 3 |
| 1 | A8 | 0 |
| 2 | BC1 | 599 |
| 3 | BC2 | 0 |
| 4 | BC3 | 459 |
| 5 | BC5 | 0 |
| 6 | R32F | 0 |
| 7 | D16 | 0 |

The B, G, R, A byte order of format 0 is confirmed by the 16x16x16 colour LUTs: red rises along x, green along y, blue
across slices. `_nrm` textures (flag bit 3, BC3) keep X in alpha and Y in green with R = B = 132 (DXT5nm layout). Some
effect textures without the flag are ordinary RGB normal maps. `_srm`, `_mtm`, `_trm` pack scalar maps into RGB. The
`_nmp` suffix does not mean normal map (logos and cube maps use it).

## FTEXS

A plain concatenation of mip blocks at the offsets given by the mip entries. When the flags have bit 0 clear the block
is the raw mip. Otherwise it is:

| part | layout |
| --- | --- |
| chunk table | chunk count x {u16 stored size, u16 unpacked size, u32 offset from the block start, bit 31 = stored raw} |
| chunks | zlib streams (78 9C), at most 0x4000 bytes unpacked each |

Every block is followed by a copy of the start of the next block (stored size + 8 x chunk count bytes); the offsets in
the mip entries account for this, so readers can ignore the extra bytes.

Cube maps: 6 faces as separate mip entries (face-major), in the order +X, -X, +Y, -Y, +Z, -Z. Volume textures: one
entry holds all depth slices one after another.

## Tiling

Texture data in the files is untiled: every mip entry's unpacked size is the linear size (BC sizes rounded up to 4x4
blocks), down to 8 bytes for a 4x4 BC1 mip, while a GCN tiled mip is at least one 8x8 micro tile; the game tiles the
data itself at upload. Linear decoding gives clean images for every format, and a 2x2-reduced mip 0 correlates with the
stored mip 1 (`ftex.py --verify`). So `ftex.py` has no detiling step.

## Converting

`python tools/ftex.py --all --game <folder with texture.qar> --chunk <extracted chunk1.psarc> --out <dir>` writes
`<pack>/<path without /Assets/>.png` plus `index.csv` per pack; textures shared by several packs are converted once
and hard-linked. Cube maps become a 6-face horizontal strip, volumes a horizontal strip of slices. One texture:
`--path /Assets/... --game <folder> --out <dir>`; a loose header: `--ftex <file>`. `--all-mips` writes every mip,
`--verify` runs the correlation check. All 1,065 textures convert; only the three `fox_pri_soil` maps come out at
their largest stored mip.
