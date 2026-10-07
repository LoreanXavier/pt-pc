# PSARC (chunk1.psarc) and Fox packages (.fpk, .fpkd)

`chunk1.psarc` is a standard PlayStation archive; `tools/psarc.py <archive> [--extract DIR]` lists or extracts it. The
packages inside are Fox Engine packages; `tools/fpk.py <package> [--extract DIR]` lists or extracts those and removes
the encryption from their entries.

## PSARC header (big-endian, 0x20 bytes)

| offset | field | chunk1.psarc |
| --- | --- | --- |
| 0x00 | magic | `PSAR` |
| 0x04 | version | 1.4 |
| 0x08 | compression | `zlib` |
| 0x0C | toc_length (includes header) | 0x3FE8 |
| 0x10 | toc_entry_size | 0x1E |
| 0x14 | toc_entries | 95 |
| 0x18 | block_size | 0x10000 |
| 0x1C | archive_flags | 0 (relative paths) |

## TOC

30-byte entries: 16-byte MD5 of the path, u32 first block index, u40 uncompressed size, u40 file offset. The block size
table follows the entries, one u16 per block (block size 0x10000 needs 2 bytes); 0 means a full uncompressed block. A
block is zlib data unless its stored size equals the block size. Entry 0 is the manifest: newline-separated paths for
entries 1..n.

## Contents of chunk1.psarc

| paths | count | format |
| --- | --- | --- |
| `as/sh/level/promotion/pt_2014/{start,hallway,hallway_maze_A,hallway_maze_B,hallway_maze_C,ending}/*.{fpk,fpkd,pftxs}` | 18 | Fox packages and texture packs, one triple per level |
| `as/sh/level/common/resident.{fpk,fpkd,pftxs}` | 3 | resident package, 63 MB fpk |
| `as/sh/level_asset/chara/player/game_object/*` | 7 | player game object, motion, parts |
| `as/sh/level/ui/subtitles/EngVoice/{Eng,Fre,Ger,Ita,Jpn,Por,Spa}Text/subtitle.{fpk,fpkd}` | 14 | subtitles, English voice only |
| `as/sh/ui/ui_default_{data,lang}.{fpk,fpkd,pftxs}` | 6 | UI |
| `as/sh/font/*.ffnt` | 4 | fonts |
| `as/sh/sound/asset/Init.bnk` | 1 | Wwise init bank |
| `as/sh/sound/asset/{bg_common,bgm_common,sfx_common,sys_resident}.sbp` | 4 | sound bank packages (bgm_common 75 MB) |
| `as/sh/demo/demo_stream/gc_p06_010_final.fsm`, `#Eng/gc_p06_010_final.fsm` | 2 | the ending cutscene stream, 6.9 MB and 14.8 MB |
| `shaders/ps4/*.fsop` | 4 | shader packs: Fx, GrModel (12 MB), GrSystem, Sh |
| `shaders/ps4/*.lua` | 8 | shader parameter packing tables |
| `as/fox/effect/gr_pic/material_params.fmtt` | 1 | material parameters |
| `silent/Recognition/GnD/EnglishUS.gnd` | 1 | voice recognition grammar, plaintext |
| `silent/Recognition/vrc/en-orbis.vrc` | 1 | voice recognition model, 3.8 MB |
| `as/sh/save/promotion/PS4/ICON_jp.png` | 1 | save icon |
| `*.lua` outside `shaders/` | 19 | scripts, all encrypted (see `foxcrypt.md`) |

Every `.lua` file starts with the Fox encryption header; the `.gnd` file does not. A level's `.fpk` holds its models,
texture headers and collision; its `.fpkd` holds the data sets (`.fox2`), scripts and effect files.
