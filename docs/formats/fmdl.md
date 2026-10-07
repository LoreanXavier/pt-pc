# FMDL (Fox model, version 2.03)

All 392 `.fmdl` files in the P.T. packages are FMDL version 2.03 (float `0x4001EB85` at offset 4), the Ground Zeroes
revision. Little-endian. Names are plain strings; the TPP hash tables (features 21 and 22) never occur. `tools/fmdl.py`
reads the format and exports glTF 2.0 (`info`, `export`, `preview`, `sheet`); `tools/meshview.py` is the software
rasterizer behind `preview` and `sheet`.

References used to start: FoxEngineTemplates `fmdl.bt` (kapuragu), FMDL-Studio-v2 (BobDoleOwndU) and pes-fmdl-blender.
Every field below was checked against the P.T. files; statements marked "guess" were not.

## Header (0x40 bytes)

| offset | type | field | P.T. |
| --- | --- | --- | --- |
| 0x00 | char[4] | magic `FMDL` | |
| 0x04 | f32 | version | 2.03 |
| 0x08 | u64 | offset of the feature and buffer tables | 0x40 |
| 0x10 | u64 | feature type bitmask, bit n set when feature n exists | |
| 0x18 | u64 | buffer type bitmask | 0xD (buffers 0, 2, 3) |
| 0x20 | u32 | feature count (popcount of the mask) | 16 to 19 |
| 0x24 | u32 | buffer count | 3 |
| 0x28 | u32 | features data offset | |
| 0x2C | u32 | features data size | |
| 0x30 | u32 | buffers data offset | |
| 0x34 | u32 | buffers data size | |

At 0x40: one 8-byte feature header per set bit, in type order (`u8 type`, `u8 count_high`, `u16 count_low`, `u32 offset`
from the features data offset; count = high * 65536 + low), then one 12-byte buffer header per set bit (`u32 type`,
`u32 offset` from the buffers data offset, `u32 size`).

Buffers: 0 = material parameter vectors (float4 array), 2 = vertex and index data, 3 = strings.

## Features

| type | content | record |
| --- | --- | --- |
| 0 | bones | 0x30 |
| 1 | mesh groups | 0x08 |
| 2 | mesh group definitions | 0x20 |
| 3 | meshes | 0x30 |
| 4 | material instances | 0x10 |
| 5 | bone groups | 0x44 |
| 6 | texture references | 0x04 |
| 7 | material parameters | 0x04 |
| 8 | shaders | 0x04 |
| 9 | vertex layouts | 0x08 |
| 10 | vertex streams | 0x08 |
| 11 | vertex elements | 0x04 |
| 12 | strings | 0x08 |
| 13 | bounding boxes | 0x20 |
| 14 | file buffers | 0x10 |
| 16 | LOD info | 0x10 |
| 17 | index slices | 0x08 |
| 18 | unknown, one u64, always 0 | 0x08 |
| 20 | unknown | 0x80 |

### Bone (0x30)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u16 | name (string index) |
| 0x02 | i16 | parent bone, -1 for the root |
| 0x04 | u16 | bounding box index |
| 0x06 | u16 | flags: 1 on almost every bone (bit 0 "has bounding box" in fmdl.bt, guess) |
| 0x08 | 8 bytes | zero |
| 0x10 | float4 | local position (w = 1) |
| 0x20 | float4 | world position (w = 1) |

No rotations. In every file local = world - parent world. The glTF export places joints at the local translations and
uses translation(-world) as inverse bind matrix.

### Mesh group (0x08)

`u16 name`, `u16 flags` (0 in P.T.), `i16 parent` (-1 root), `i16 unknown` (always -1).

### Mesh group definition (0x20)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | zero |
| 0x04 | u16 | mesh group |
| 0x06 | u16 | mesh count |
| 0x08 | u16 | first mesh |
| 0x0A | u16 | bounding box index |
| 0x0C | u32 | zero |
| 0x10 | u16 | first index slice (first mesh * LOD count) |
| 0x12 | 14 bytes | zero |

Several definitions can use the same group.

### Mesh (0x30)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | render flags |
| 0x04 | u16 | material instance |
| 0x06 | u16 | bone group |
| 0x08 | u16 | vertex layout (equals the mesh index in P.T.) |
| 0x0A | u16 | vertex count |
| 0x0C | u16 | vertex start, always 0 |
| 0x0E | u16 | zero |
| 0x10 | u32 | first index, in index units |
| 0x14 | u32 | index count of LOD 0 |
| 0x18 | u32 | first index slice |
| 0x1C | 20 bytes | zero |

Render flags: bit 4 selects the depth bias states, with bits 0-3 as the bias level; bit 5 no back-face culling; bit 6
the other depth state group; bit 7 alpha test, dithered unless bit 14 is set too, which gives a constant reference;
bits 9 and 10 keep a mesh out of the opaque and alpha-tested passes. A mesh with bit 6, bit 8 or a depth bias level
above 0 casts no shadow; the shadow passes do not look at the material, so forward (glass, emissive) meshes without
those bits cast shadows too. The exporter maps bit 5 to glTF `doubleSided` and bit 7 to `alphaMode MASK`, and keeps
the raw value in the primitive and material extras.

### Material instance (0x10)

`u16 name`, `u16 zero`, `u16 shader` (feature 8 index), `u8 texture parameter count`, `u8 vector parameter count`,
`u16 first texture parameter`, `u16 first vector parameter`, `u32 zero`.

### Material parameter (0x04)

`u16 name` (for example `Base_Tex_SRGB`, `NormalMap_Tex_NRM`, `SpecularMap_Tex_LIN`, `MatParamMap_Tex_LIN`,
`MatParamIndex_0`, `URepeat_UV`), `u16 reference`: a texture reference index for the instance's texture parameters, a
buffer 0 vector index for its vector parameters.

`MatParamIndex_N` vectors hold an integer in x (0 to 58, one 256) and zeros. `as/fox/effect/gr_pic/material_params.fmtt`
is 8192 bytes, 256 rows of two float4; the index most likely selects a row there (guess).

### Texture reference (0x04)

`u16 file name` (`shsb_hous001_a1_bsm.tga`; extensions .tga, .psd, .dds), `u16 directory`
(`/Assets/sh/environ/object/shsb/house/shsb_hous001/sourceimages/`). The game file is directory + stem + `.ftex`.

### Shader (0x04)

`u16 shader`, `u16 technique`, for example `fox_3ddf_basic_nrmuv` / `fox3DDF_Blin_LNM`. 41 pairs occur.

### Vertex layout, stream, element

Layout (0x08): `u8 stream count`, `u8 element count`, `u8 unknown` (0), `u8 UV set count`, `u16 first stream`,
`u16 first element`.

Stream (0x08): `u8 file buffer`, `u8 element count`, `u8 stride`, `u8 slot`, `u32 byte offset` of the mesh's data in the
file buffer. A layout's elements are handed out to its streams in order.

Element (0x04): `u8 usage`, `u8 format`, `u16 byte offset` inside one stream vertex.

| usage | meaning | | format | meaning |
| --- | --- | --- | --- | --- |
| 0 | position | | 1 | R32G32B32_FLOAT |
| 1 | bone weights | | 6 | R16G16B16A16_FLOAT |
| 2 | normal | | 7 | R16G16_FLOAT |
| 3 | color | | 8 | R8G8B8A8_UNORM |
| 7 | bone indices | | 9 | R8G8B8A8_UINT |
| 8 to 11 | UV0 to UV3 | | | |
| 14 | tangent | | | |

Slot 0 is always a position stream (file buffer 0, stride 12). Slots 1, 2 and 3 all use file buffer 1 with the same
offset, so they describe one interleaved vertex: slot 1 normal and tangent, slot 2 color, slot 3 UVs and skin data.

| layouts | file buffer 1 vertex (offset: element) | stride |
| --- | --- | --- |
| 893 | 0 normal, 8 tangent, 16 UV0 | 20 |
| 357 | 0 normal, 8 tangent, 16 weights, 20 indices, 24 UV0 | 28 |
| 18 | as the first, UV1 shares UV0's offset 16 | 20 |
| 7 | other UV1/UV2 variants, one with a color at 16 | 24 to 32 |

Normal w is 1 in every mesh, tangent w is +1 or -1. Bone indices index the mesh's bone group; the four weight bytes sum
to 255 for every skinned vertex.

### Other records

| feature | layout | P.T. |
| --- | --- | --- |
| 12 string | `u16 buffer type` (3), `u16 length`, `u32 offset` | string 0 is empty |
| 13 bounding box | float4 max, float4 min | box 0 is the extent of all vertices |
| 14 file buffer | `u16 type` (0 vertices, 1 indices), `u16` 0, `u32 size`, `u32 offset` in buffer 2, `u32` 0 | always: positions, interleaved attributes, indices |
| 16 LOD info | `u16 LOD count`, `u16` 0, 3 floats | count 8, 4 or 1; the floats look like LOD switch thresholds (guess) |
| 17 index slice | `u32 start`, `u32 count` | see LODs |
| 5 bone group (0x44) | `u16 max weights per vertex`, `u16 bone count`, `u16 bones[32]` | max weights 1 to 4 |
| 20 (0x80) | `u32` 0, 3 floats, 3 floats (0), `u32` 0xFFFFFFFF, zeros | first floats vary per model, meaning unknown |

## Indices and LODs

The index buffer (file buffer type 1) holds u16 triangle lists. Mesh m owns `LOD count` slices from its first index
slice. Slice 0 is `(0, index count)`; slice k is `(start, count)` with start relative to the mesh's first index. LOD
ranges overlap and reuse indices, and every range stays below the next mesh's first index.

## Orientation

- Meters, +Y up, right-handed; the same frame as glTF, so positions are exported unchanged.
- Front faces are clockwise. The exporter swaps the second and third index to get glTF's counter-clockwise order.
- UV origin is top-left, used unchanged in glTF.
- Level models are authored in level space: `shsb_hous001` (the corridor) spans x -13.9 to 15.3, z -27 to -10.5.

## Export

`python tools/fmdl.py export <package|dir|file> --out <dir>` writes `<name>.gltf`, `.bin` and a `.json` sidecar (groups,
meshes with vertex formats and LOD ranges, materials with shader, technique, texture `.ftex` paths and vectors, bones),
plus `index.json` and `textures.json` per package. Base color PNGs converted by `tools/ftex.py` are linked when they are
found under the matching paths (`--no-textures` to skip). `info` prints the tables, `preview` renders PNGs with a
software rasterizer (shaded, normals, UV checker, textured), `sheet` one thumbnail per model.

## P.T. inventory

| package | models | vertices | triangles |
| --- | --- | --- | --- |
| pt14_hallway | 113 | 220,779 | 285,837 |
| pt14_hallway_maze_A | 41 | 183,907 | 209,743 |
| pt14_hallway_maze_B | 52 | 137,457 | 155,787 |
| pt14_hallway_maze_C | 83 | 195,137 | 245,460 |
| pt14_start | 6 | 18,906 | 23,799 |
| ending | 72 | 311,766 | 257,480 |
| resident | 23 | 118,347 | 179,298 |
| plparts_normal | 2 | 52,480 | 88,431 |

Skinned models (bones): `bab0` fetus (28), `och0` Lisa (90), `plr0` player (191), `dol0` (74), `hsh0` (74), `coc0`
cockroach (31), `frz0` hanging fridge (3), `lte0` hanging lamp (3), `pab0` paper bag (4), `shl0` and `lig0` flashlight
(1), and four props in `ending`.
