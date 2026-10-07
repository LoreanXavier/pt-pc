# GEOM (collision, FoxData version 201209110)

All 355 `.geom` files in the P.T. packages use the FoxData container, version 201209110, little-endian. Each sits next
to the `.fmdl` of the same name, and most of its polygons index that model's vertex positions. `tools/geom.py` reads the
format and exports triangles to glTF and OBJ (`info`, `export`, `preview`, `sheet`).

References used to start: FoxEngineTemplates `geom_geoms_gskl.bt`, `geo_common.bt` and `FoxData_common.bt` (TPP and
PES). The layout below was checked against the P.T. files.

## Header (0x20 bytes)

| offset | type | field | P.T. |
| --- | --- | --- | --- |
| 0x00 | u32 | version | 201209110 |
| 0x04 | u32 | offset of the first node | 0x20 |
| 0x08 | u32 | file size | |
| 0x0C | u32 | name hash | 0x006B936E (`GEOM` in the templates' table) |
| 0x10 | u32 | name string offset | 0 |
| 0x14 | u32 | flags | 0 |
| 0x18 | u64 | content hash (guess): equal exactly when files are byte-identical | |

## Nodes (0x28 bytes, 0x30 apart)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | name hash |
| 0x04 | u32 | name string offset (0) |
| 0x08 | u32 | flags |
| 0x0C | i32 | payload offset from the node |
| 0x10 | u32 | payload size |
| 0x14 | i32 | parent offset (0) |
| 0x18 | i32 | first child offset |
| 0x1C | i32 | previous sibling offset |
| 0x20 | i32 | next sibling offset |
| 0x24 | i32 | parameters offset (0) |

The tree has at most these two top-level nodes, each with one group node as its only child:

| node | flags | name hash | child | content |
| --- | --- | --- | --- | --- |
| 0 | 0x10 | differs per file | group node, flags 6, hash 0xBE9A4BB7 | coarse hull with its own vertices, tagged CHARA and PLAYER |
| 1 | 0x40 | 0x0A7192FA | group node, flags 6, hash 0xBE9A4BB7 | detailed polygons on the FMDL vertices, tagged BULLET and CAMERA |

Both groups with data in 106 files, only node 1 in 241, node 0 only in 5, empty in 3 (no collision). The node, shape
and material hashes are not StrCode32 of any known string; they stay unresolved.

## Group payload

A table of 0x20-byte blocks. The first byte of an entry is 1 on the terminator entry and 0 otherwise. After the
terminator comes the material section.

| offset | type | field |
| --- | --- | --- |
| 0x00 | u8 | 0 (1 on the terminator) |
| 0x01 | u8 | shape count |
| 0x02 | u16 | size of the shape and vertex data |
| 0x04 | u16 + u16 | legacy vertex header offset, unreliable |
| 0x08 | u16 + u16 | legacy shape offset, unreliable |
| 0x0C | u32 | vertex header offset from the block |
| 0x10 | u32 | first shape offset from the block |
| 0x14 | u32 | offset from the block to the material section |
| 0x18 | u64 | collision tags of the block |

Groups hold 1 to 187 blocks. All POLY shapes of a block share the block's vertex header; in node 1 that header selects
one FMDL mesh, and large meshes span several blocks.

Material section: `u8 first`, `u8 total` (entries including a zero terminator), `u8 aux first`, `u8 aux total`, then
12-byte entries (`u32 material hash`, `u64` 0). 34 groups carry 1 to 4 materials; 15 distinct hashes occur, unresolved.
The executable's strings `MTR_WOOD_G`, `MTR_TILE_A`, `MTR_GLAS_C`, `MTR_CONC_A`, `MTR_WOOD_A`, `MTR_WATE_A`, `MTR_NONE_A`
are likely the names behind them (guess).

## Shapes (0x20-byte header, primitives follow)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | bits 0-3 primitive type, bits 4-23 flags, bits 24-31 primitive count |
| 0x04 | u32 | next sibling, 28-bit signed, in 16-byte units from this shape |
| 0x08 | u32 | previous sibling, same encoding |
| 0x0C | u32 | first child, same encoding |
| 0x10 | u64 | collision tags |
| 0x18 | u32 | name hash (0 on AABB shapes) |
| 0x1C | u32 | vertex header, 28-bit signed, 16-byte units from this shape |

Only two primitive types occur. Each block is a two-level tree: an AABB root, whose child list holds AABB nodes, each
with one POLY child.

| type | primitive | size | layout |
| --- | --- | --- | --- |
| 4 | AABB | 0x20 | float3 half extents, pad, float3 center, pad |
| 2 | POLY | 0x0A | `u16 a, b, c, d`, `u16 info` |

Shape flags seen: AABB always 0x2000 (has child). POLY: 0x800 (vertices from the FMDL), 0x0 (own vertices), and 0x4A00,
0x4800, 0xA00, 0x200 on a few. Template names: 0x200 double sided, 0x800 FMDL vertices, 0x4000 unknown.

Poly indices are unsigned (meshes above 32,767 vertices use the top bit). A polygon is the quad a, b, c, d; when d
equals a it is the triangle a, b, c. The exporter emits a, b, c and a, c, d. Vertex order is counter-clockwise around
the outward normal, the opposite of FMDL index order.

Poly info: bit 0 no material, bit 1 no auxiliary material, bits 2-8 material index into the group's material list,
bits 9-15 auxiliary material index (0x7F = none). Almost every polygon has 0xFFFF; 1,867 polygons carry materials 0 to 3.

## Vertex header (0x20 bytes)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | vertex count |
| 0x04 | u32 | index of the first polygon vertex in the vertex table |
| 0x08 | u32 | 1 on own-vertex headers, 0 on FMDL ones ("add origin", guess) |
| 0x0C | u32 | origin index in the vertex table |
| 0x10 | i64 | vertex table offset from this header (0 on FMDL ones; a runtime pointer per the templates) |
| 0x18 | u32 | byte offset into the FMDL position buffer (file buffer 0) |
| 0x1C | u32 | 0 |

- FMDL vertices (shape flag 0x800): positions are the FMDL's float3 positions (stride 12) from the given byte offset;
  offset and count equal the position stream offset and vertex count of one FMDL mesh. Polygon indices are mesh-local.
- Own vertices: a float4 table (w = 0) of `count` entries. Entry `origin index` is (0, 0, 0) in every file, polygons
  index from `index offset` (always 1).

## Collision tags

A 64-bit mask per block and per shape. Bit names follow the TPP `GEO_COL_A` table in `geo_common.bt`; they are guesses
for P.T.

| tags | bits (TPP names) | node |
| --- | --- | --- |
| 0x0080000000842DC2 | RECOIL BULLET MISSILE BOMB BLOOD IK STOP_EYE CAMERA BULLET_MARK MARKER | 1 |
| 0x0080000020842DC2 | same plus NO_WALL_MOVE (bit 37) | 1 |
| 0x006000008000003C | CHARA SOUND PLAYER ENEMY HORSE VEHICLE | 0 |
| 0x0000000000000094 | CHARA PLAYER MISSILE | 0 |

Node 0 is the movement hull for the player; node 1 is the detailed surface for line checks such as the camera's. Some
hulls carry extra geometry (the bathroom hull includes a 0.1 m cube at the level origin); that is in the data, not a
decode error.

## Export

`python tools/geom.py export <package|dir|file> --out <dir>` writes `<name>.gltf`, `.bin`, `.obj` and a `.json` sidecar
(node tree, blocks, tags, materials), plus `index.json` per package. Triangles are grouped by node, tags and material;
glTF node extras and OBJ comments carry them. `info -v` prints the shape trees.

## P.T. inventory

| package | files | triangles |
| --- | --- | --- |
| pt14_hallway | 110 | 255,196 |
| pt14_hallway_maze_A | 41 | 208,659 |
| pt14_hallway_maze_B | 52 | 155,068 |
| pt14_hallway_maze_C | 83 | 251,940 |
| pt14_start | 6 | 25,359 |
| ending | 63 | 174,398 |
