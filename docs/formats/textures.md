# Textures: FTEX, FTEXS, PFTXS, texture.qar, pathid_list_ps4.bin

Tools: `tools/ftex.py` (convert to PNG), `tools/qar.py` (QAR reader), `tools/pathcode.py` (PathCode64, pathid list). Addresses are eboot.elf virtual addresses (base 0x400000), function names in the last section.

## Where the pixel data lives

A texture is one FTEX header (`.ftex`) plus one to six FTEXS files (`.1.ftexs` .. `.6.ftexs`) that hold the mips.

- `texture.qar` holds the complete set, header and every FTEXS file, for 1,061 textures.
- Each `.pftxs` pack (one per level, plus resident, player parts and UI) holds copies of the FTEX headers of its textures and of the low resolution FTEXS files. The two highest resolution files (mips 0 and 1; only mip 0 for 2-file textures) are only in `texture.qar`. Every copy in a pack is byte-identical to the one in `texture.qar` (checked for all 2,021 pack entries that exist in both).
- 4 textures are not in `texture.qar`: `/Assets/tpp/common_source/flat/Pictures/cm_flat_white` (complete in `ui_default_data.pftxs`) and `/Assets/fox/environ/object/sourceimages/fox_pri_soil_{bsm,nrm,srm}` in `ui_default_lang.pftxs`, whose mips 0 and 1 exist nowhere in the game files.
- 57 textures are only in `texture.qar` (effects, noise, load icon).
- The 27 loose `.ftex` files inside the fpk packages are the same headers again; their data is in `texture.qar`.

Pixel data is stored untiled (see Tiling).

## PathCode64

`texture.qar` and `pathid_list_ps4.bin` key files by a 64-bit path code. Code at 0xA23950 (path) and 0xA23AA0 (extension), 0x10A2EF0 (flag):

```
stem, ext   = path split at the first '.' after the last '/'
text        = stem without a leading "/Assets/"
seed1       = little-endian u64 of the last 8 bytes of text, reversed (last byte lowest)
hash(s)     = HashLen16(CityHash64(s) - 0x9AE16A3B2F90404F, seed1(s))
type        = hash(ext without '.') & 0x1FFF
code        = type << 51 | meta << 50 | hash(text) & 0x3FFFFFFFFFFFF
meta        = 0 for "/Assets/<root>/..." with root in {fox, tpp, sh, mgo} (pointer table 0x1B8F580), else 1
```

This is the same scheme as GzsTool `HashFileNameWithExtension` (MGSV). All 4,955 codes of `pathid_list_ps4.bin` recompute from their paths (`pathcode.py --list ... --verify`), with `/app0/as/` read as `/Assets/`.

CityHash64 here is v1.0.3 (0xA23170, unaligned copy 0xA22C80). For inputs longer than 64 bytes it differs from `tools/foxhash.py cityhash64` (x = Fetch64(s + len - 40), y = Fetch64(s + len - 16) + Fetch64(s + len - 56), z = HashLen16(Fetch64(s + len - 48) + len, Fetch64(s + len - 24)) ...). StrCode64 (0xA23800) calls the same function, so `foxhash.strcode64` is wrong for strings of 64 or more characters. `pathcode.cityhash64` implements the long path.

Extension types seen:

| type | ext | | type | ext |
| --- | --- | --- | --- | --- |
| 685 | ftex | | 164 | fmtt |
| 5720 | 1.ftexs | | 796 | lua |
| 5806 | 2.ftexs | | 1439 | fsop |
| 2787 | 3.ftexs | | 1677 | vrc |
| 2038 | 4.ftexs | | 1740 | ffnt |
| 4971 | 5.ftexs | | 1752 | bnk |
| 2084 | 6.ftexs | | 1977 | tga (`save/promotion/PS4/ICON_jp`, the psarc has a .png) |
| 5727 | pftxs | | 2629 / 7594 | fpk / fpkd |
| 3131 | fsm | | 3243 / 5980 | gnd / sbp |

## pathid_list_ps4.bin

Loaded by 0x4446F0 at boot. Maps every shipped file's PathCode64 back to its path string: 0x10A96D0 binary searches the code table, 0x10A9770 builds `dir + "/" + name`. Field names partly from kapuragu FoxEngineTemplates `pathid_list_ps4_bin.bt`. Little-endian, every table starts 16-aligned.

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

A Fox QAR archive without a header. Mounted as device "Texture" (0xD14570 -> 0xDB2190 -> 0x436BA0). Opened by 0x437620: seek to end - 0x24, read the footer, check the magic, read the entry table; entries are then sorted by code and found by binary search (0x437CD0). Data offset = `offset16 << 4` (0x437010, 0x437120, 0x437C40).

Footer, last 0x24 bytes:

| offset | type | field | value |
| --- | --- | --- | --- |
| 0x00 | u64 | unknown; 0x437AC0 returns it instead of the table offset when flag bit 1 is set | 0x352E1438 |
| 0x08 | u64 | unknown | 0 |
| 0x10 | u32 | entry count | 4862 |
| 0x14 | u16 | flags: bit 0 = NUL-terminated names follow the entry table, bit 1 = see 0x00 | 0 |
| 0x16 | u16 | magic | 0x7161 |
| 0x18 | u32 | entry table offset >> 4 | 0x0352E17E |
| 0x1C | u32 | unknown | 0 |
| 0x20 | u32 | unknown | 0x14 |

Entry, 16 bytes: u64 PathCode64, u32 data offset >> 4, u32 size. In the file the entries are in data order: files sorted by path, and per texture `.1.ftexs`, `.2.ftexs`, ..., then `.ftex`. Every file starts 16-aligned; the data area is contiguous from 0 to the table.

## PFTXS

One pack per level. Parser logic is from the data; all 10 packs parse completely with it.

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

After the last record: `EOPF`, then 0xCC padding to a multiple of 0x800.

Files per pack (FTEX `ftexs count` n, PSUB k): n=1 k=1 (102), n=2 k=1 (275), n=3 k=1 (363), n=4 k=2 (510), n=5 k=3 (715), n=6 k=4 (59). So a pack always has mip 2 and below (or everything below mip 0 for n=2, everything for n=1).

## FTEX (version 2.03)

Names partly from FoxEngineTemplates `ftex.bt`; the header reader is 0x1039010, the mip reader 0x1039710, the writer 0x10381B0.

| offset | type | field | values in P.T. |
| --- | --- | --- | --- |
| 0x00 | char[4] | `FTEX` (`XETF` = big-endian, swapped by 0x1039010) | |
| 0x04 | f32 | version | 2.03 |
| 0x08 | u16 | pixel format, see below | 0, 2, 4 |
| 0x0A | u16 | width | 16..4096, all powers of two except 256x2 |
| 0x0C | u16 | height | |
| 0x0E | u16 | depth | 1, 16 (two 16x16x16 LUTs) |
| 0x10 | u8 | mip count | |
| 0x11 | u8 | filter (0 point, 1 linear/point mip, 2 trilinear; passed to the sampler by 0xD302B0) | 2, 0 once |
| 0x12 | u16 | address mode, one nibble per axis (guess from values; 0xD302B0 maps only 0, 0x111, 0x222, 0x333 = clamp, wrap, mirror, border) | 0x11, 0x0, 0x111 |
| 0x14 | u16 | unknown | 1 |
| 0x16 | u16 | unknown | 0 |
| 0x18 | u32 | unknown | 0 |
| 0x1C | u32 | flags | see below |
| 0x20 | u8 | FTEXS file count | 1..6 |
| 0x21 | u8 | first mip stored in `.1.ftexs` | = count - 1 |
| 0x22 | 14 bytes | zero | |
| 0x30 | 16 bytes | unknown hash, unique per texture content | |
| 0x40 | 16 bytes x mips x faces | mip entries, face-major (face 0 mips 0..n-1, then face 1 ...) | |

Flags: bit 0 = mips stored as zlib chunks (0x1039710), bit 1 = sRGB (FoxEngineTemplates; set on all 273 `_bsm` colour maps and on no `_nrm`, `_srm`, `_trm`, `_mtm` map), bit 2 = cube map, 6 faces (0x1039010, 0x10381B0), bit 3 = normal map (0xD74A30 checks it with format 4), bit 24 = mips in external `.N.ftexs` files. Seen: 0x1000001 (508), 0x1000003 (273), 0x1000009 (267), 0x1000007 (8 cube maps), 0x1000000 (2), 0x1000008 (2), 0x1000002 (1).

Mip entry:

| offset | type | field |
| --- | --- | --- |
| 0x0 | u32 | offset in the FTEXS file (or after the header in the .ftex when the file number is 0) |
| 0x4 | u32 | unpacked size |
| 0x8 | u32 | stored size: 0 when not chunked, else chunk table + chunk data padded to 16 |
| 0xC | u8 | mip level |
| 0xD | u8 | FTEXS file number, 0 = inside the .ftex |
| 0xE | u16 | chunk count |

File assignment (writer 0x10381B0 with 0x1037490): starting from the smallest mip, mips go into `.1.ftexs` while their running total is at most 0x8000 bytes; each larger mip gets its own file, so mip 0 is in the highest-numbered file.

Pixel formats (bits per pixel from 0x1039010, GNM formats from 0xD302B0 and 0xD949F0):

| value | format | textures in texture.qar |
| --- | --- | --- |
| 0 | B8G8R8A8 (GNM 8_8_8_8 with channel swizzle Z, Y, X, W: bytes are B, G, R, A) | 3 |
| 1 | A8 | 0 |
| 2 | BC1 | 599 (602 with the pack-only ones) |
| 3 | BC2 | 0 |
| 4 | BC3 | 459 (460) |
| 5 | BC5 | 0 |
| 6 | R32F | 0 |
| 7 | D16 | 0 |

The B, G, R, A byte order of format 0 is confirmed by the 16x16x16 colour LUTs (`lut/*_FILTERLUT`): red rises along x, green along y, blue across slices.

Channel use seen in the decoded data: `_nrm` textures (flag bit 3, BC3) keep X in alpha and Y in green with R = B = 132 (DXT5nm layout). Some effect textures without the flag are ordinary RGB normal maps (for example `fx_glscom03a_ks_clp`). `_srm`, `_mtm`, `_trm` pack scalar maps into RGB. The `_nmp` suffix does not mean normal map (logos and cube maps use it).

## FTEXS

A plain concatenation of mip blocks at the offsets given by the mip entries. When the flags have bit 0 clear the block is the raw mip. Otherwise it is:

| part | layout |
| --- | --- |
| chunk table | chunk count x {u16 stored size, u16 unpacked size, u32 offset from the block start, bit 31 = stored raw} |
| chunks | zlib streams (78 9C), at most 0x4000 bytes unpacked each |

The writer (0x10381B0) writes each block with stored size + 8 x chunk count bytes, so every block is followed by a copy of the start of the next block in memory. The offsets in the mip entries account for this; readers can ignore the extra bytes.

Cube maps: 6 faces as separate mip entries (face-major). The decoded faces show ceiling and floor at faces 2 and 3 of `cubemap_hallway01`, so the order is +X, -X, +Y, -Y, +Z, -Z (the usual GNM and D3D order). Volume textures: one entry holds all depth slices one after another.

## Tiling

Texture data in the files is untiled. Evidence:

1. All 9,293 mip entries of the 1,061 texture.qar textures have unpacked size = linear size (BC sizes rounded up to 4x4 blocks), down to 8 bytes for a 4x4 BC1 mip. A GCN 1D or 2D tiled mip is at least one 8x8-element micro tile.
2. 0x1049060 uploads a mip by computing its surface offset (0x105D250), then calling `sce::GpuAddress::tileSurface` (0x105DDE0, assert strings `tiler.cpp` / `tileSurface`) on the loaded linear data, or a plain copy for linear-mode textures. 0xD91600 calls it for every mip and slice. 0x1048AE0 does the same for initial data at texture creation. 0x1049160 is the reverse (0x105E380, detile).
3. Linear decoding gives clean images for every format and a 2x2-reduced mip 0 correlates with the stored mip 1 (median 0.982 over 994 textures, `ftex.py --verify`); the few low scores are pure noise textures and near-constant channels.

So `tools/ftex.py` has no detiling step. A GCN detiler is only needed for GPU memory dumps, not for asset files.

## Conversion results

`python tools/ftex.py --all --verify` writes `dump/textures/<pack>/<path without /Assets/>.png` plus `index.csv` per pack; textures shared by several packs are converted once and hard-linked. Cube maps become a 6-face horizontal strip, volumes a horizontal strip of slices. Other options: `--path /Assets/...` (one texture from texture.qar), `--ftex file.ftex` (loose header), `--all-mips`, `--dry-run`.

| pack | textures | ok | partial |
| --- | --- | --- | --- |
| ending | 464 | 464 | 0 |
| plparts_normal | 59 | 59 | 0 |
| pt14_hallway | 412 | 412 | 0 |
| pt14_hallway_maze_A | 227 | 227 | 0 |
| pt14_hallway_maze_B | 192 | 192 | 0 |
| pt14_hallway_maze_C | 342 | 342 | 0 |
| pt14_start | 66 | 66 | 0 |
| resident | 200 | 200 | 0 |
| ui_default_data | 59 | 59 | 0 |
| ui_default_lang | 3 | 0 | 3 (fox_pri_soil, mip 2 is the largest stored) |
| texture_qar_only | 57 | 57 | 0 |

2,081 rows, 1,065 unique textures, no failures.

## eboot functions

| address | name | confidence |
| --- | --- | --- |
| 0x4446F0 | PathIdList_Load (`pathid_list_ps4.bin`) | confirmed (string) |
| 0x10A96D0 | PathIdList_FindPath (binary search code -> path) | likely |
| 0x4450B0 | Path_ToCode (normalize, then 0x4359D0) | likely |
| 0x10A2EF0 | PathCode64_FromString (meta flag rules) | likely |
| 0xA23950 | PathCode64_Hash | confirmed (matches all pathid codes) |
| 0xA23AA0 | PathCode64_ExtensionType | confirmed |
| 0xA23CD0 | PathCode64_WithExtension | confirmed |
| 0xA23170 / 0xA22C80 | CityHash64 v1.0.3, aligned / unaligned input | confirmed |
| 0xA23800 | StrCode64 | likely |
| 0x436BA0 | QarDevice_Mount(name, path) | likely |
| 0x437620 | QarFile_Open (footer, table) | confirmed |
| 0x437CD0 | QarFile_Find (binary search) | confirmed |
| 0x437010 / 0x437120 | QarDevice_Lookup (path, offset << 4, size) | likely |
| 0xD14570 | TextureStreamer_Init (mounts `texture.qar`) | likely |
| 0xD17E50 | Ftex_MakeFileName (`.ftex`, `.N.ftexs`) | confirmed (strings) |
| 0x1039010 | FtexReader_Init (endian swap, bits per pixel) | likely |
| 0x1039710 | FtexReader_ReadMip (raw copy or zlib chunks) | confirmed |
| 0x10381B0 | Ftex_Write (dev converter) | likely |
| 0x1037490 | Ftex_AssignFtexsFiles | likely |
| 0xD302B0 | Texture_LoadFromLooseFtex (dev path) | likely |
| 0x1048AE0 | GnTexture_Create (tiles initial data) | likely |
| 0x1049060 | GnTexture_UploadMip (tileSurface) | likely |
| 0x1049160 | GnTexture_ReadbackMip (detileSurface) | likely |
| 0x105DDE0 | sce::GpuAddress::tileSurface | confirmed (assert strings) |
| 0x105E380 | sce::GpuAddress::detileSurface | guess |
