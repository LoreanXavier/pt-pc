# UI data

The files behind what P.T. draws on top of the 3D scene: fonts (`.ffnt`), language files (`.lng`), subtitle entries
(`.subp`), UI models (`.uif`), layouts (`.uilb`), animations (`.uia`) and UI graphs (`.uigb`). Tools: `tools/ffnt.py`
(glyph table, text render), `tools/lng.py` (dump as JSON), `tools/uif.py` (dump as JSON; `--dict` names the hashed
node, font and material names).

## Files

| data | package | use |
| --- | --- | --- |
| `/Assets/sh/font/font_def_ltn.ffnt`, `font_def_jp.ffnt` | chunk1 | UI font type 0 (menus); Japanese uses `_jp` |
| `/Assets/sh/font/LatinFont.ffnt`, `KanjiFont.ffnt` | chunk1 | UI font type 1, "movie font" (subtitles); Japanese uses Kanji |
| `/Assets/sh/lang/ui/OPTIONS.lng#<lang>`, `SYSTEM.lng#<lang>` | `ui_default_lang.fpkd` | menu and system texts, lang = eng fra deu spa jpn ita por |
| `/Assets/sh/ui/GraphAsset/Common/data/common_art.fox2` | `ui_default_data.fpkd` | `common_fnt_palette` (menu font sizes), `common_col_palette` |
| `/Assets/sh/ui/common_subtitle.fox2` | `ui_default_data.fpkd` | `subtitles_fnt_palette` (`sbt-sys-M`, per language) |
| `/Assets/sh/ui/ModelAsset/sys_subtitle/Scenes/UI_sys_subtitle.uif` | `resident.fpk` | subtitle text node |
| `/Assets/sh/ui/ModelAsset/sys_option/Scenes/UI_sys_option.uif`, `UI_sys_opt_*.uia`, `LayoutAsset/sys_option/UI_sys_option.uilb` | `ui_default_data.fpk` | option (pause) menu |
| `/Assets/sh/ui/Subtitles/subp/EngVoice/<Lang>Text/trial.subp` | `subtitle.fpk` per language | subtitles and puzzle captions |
| `/Assets/sh/effect/vfx_pic/text/text_sub01_alp` .. `text_sub10_alp`, `noise/Noise_00007`, `holl/holl_002_alp` | resident, texture.qar | subliminal messages and the peephole overlay |
| `/Assets/sh/ui/GraphAsset/opening.uigb`, `bug_expression.uigb`, layouts `sys_opening`, `sys_bug`, `sys_trophy` | `resident.fpk` | the preface and the fake crash screen |
| `/Assets/sh/ui/GraphAsset/promotion_ending.uigb`, layouts and models `sys_ending`, `sys_logo` | `ending.fpk` | ending narration, credits and logos |
| `/Assets/sh/ui/ModelAsset/*/Pictures/*` | texture.qar | all UI pictures |

## FFNT

Little endian. Header: `FXFT`, u16 1, u8 entry count (2), u8 0, u16 10; entry table at 0x10 of {char[4] tag, u32 offset,
u32 size}.

`GLYP` entry: 16-byte header, then u16-sorted glyph records of 20 bytes.

| offset | type | field |
| --- | --- | --- |
| +0x02 | u8 | em size in bitmap pixels (53 `font_def_*`, 54 Latin/Kanji) |
| +0x06 | u16 | glyph count |
| +0x08 | u32 | record bytes |
| +0x0C | u8 | pad: extra advance on each side of a glyph, font units (0 `font_def_*`, 8 Latin/Kanji) |

Glyph: u32 code point (low 28 bits), u16 x, u16 y, u8 width, u8 height, u8 layer (bit plane), u8 advance, u8 bearing
(left), s8 top (from the line top), u16 flags (bit 0 and 1 each remove one pad from the advance), u32 0.

`FTDT` entry: u8 0, u8 log2 width (10), u8 log2 height (9), u8 8 (bit planes), u32 size, 8 zero bytes, then
width x height bytes; glyph pixels are bit `1 << layer` of each byte (all P.T. glyphs use layer 0).

The game copies a glyph into its cache texture through a 3x3 kernel (1 2 1 / 2 4 2 / 1 2 1, /16) and samples it
bilinear, with the UV rect half a texel past the glyph cell on every side while the quad keeps the glyph's size, so a
glyph shows w / (w + 1) of its nominal scale. Advance = `fontWidth * (advance + 2 pad - flag pads) / em + textSpace` per
character; line height = `fontHeight * (em + 2 pad) / em`, plus `lineSpace` between lines. The glyph quad sits at
`pen + fontWidth * (pad + bearing) / em`, `lineTop + fontHeight * (pad + top) / em`. Line breaks: LF, CR LF; a run wider
than the box breaks at the last space, otherwise at the character.

Font styles (`UiFontDataElement` in the palettes):

| name | language | width x height | textSpace | lineSpace |
| --- | --- | --- | --- | --- |
| `cmn-cmn-sys-M` | all | 20 x 20 | -1 | 0 |
| `cmn-cmn-sys-L` | all | 24 x 24 | -1 | 0 |
| `sbt-sys-M` | default | 22 x 22 | -6 | -4 |
| `sbt-sys-M` | jpn | 22 x 22 | -7 | -4 |

The `sbt` spacing cancels the 16-unit pad of the movie fonts, which is why subtitles use font type 1 and menus type 0.
The `fontEdge` field of the element is not read by any draw path; the subtitles' dark rim comes from their shader
(below).

## LANG (.lng, version 2)

`LANG`, u32 version 2, char[4] `LE\0\0`, u32 count, u32 entry table, u32 key strings, u32 values, u32 0. Entry: {u32 key
offset, u32 value offset}. Key: ASCII, zero terminated. Value: u16 color id (1 subtitle-like texts, 0x101 option
texts, 0x303 system texts), then UTF-8 text, zero terminated. `OPTIONS.lng` also carries `TRIA1000_*` copies of the
subtitle texts (the subtitle system reads `.subp`).

## SUBP

`audio.md` has the index layout. Entry header (12 bytes; field names from Atvaark's SubpTool):

| offset | type | field |
| --- | --- | --- |
| +0x00 | u16 | 0x4C01 |
| +0x02 | u8 | line (timing) count |
| +0x03 | u8 | category: 5 in-game voice, 7 super (caption) |
| +0x04 | u16 | text bytes including the terminator |
| +0x06 | u16 | text bytes (+ additional length) |
| +0x08 | u16 | character id (2 caster, 0xF the bag, 0xD baby and ending voice, 1 captions) |
| +0x0A | u8 | range: index into the radius table {0, 20, 40, 70} m; 0 and anything above 3 set no distance limit |
| +0x0B | u8 | 1 |

Timings are u16 pairs in 1/100 s. Text lines are separated by `$`; CR LF inside a line is a line break. Encoding: eng,
jpn and por are UTF-8, fra, deu, spa and ita are Windows-1252. Every P.T. voice entry has range 0 and every caption
0xFF, so no P.T. subtitle is hidden by distance; the radio lines are limited by a trap box instead
(`trapSubtitleVisibleArea.lua`).

The puzzle captions are entries of category 7 played by message id (the low 32 bits of StrCode64 of the lowercase
entry id, like every `.subp` key): `tria1000_1k1010` (Forgive me, Lisa / There's a monster inside of me),
`tria1000_1l1010`, `tria1000_1m1010`, `tria1000_1j1010`, and `tria1000_141010` (the ending voice, 9 lines).

### Subtitle layout

`SubtitlesGenerator` in `subtitle_boot.fox2`: color 1 1 1 1, offset (0, -16), size (27, 38), hAlign left, vAlign bottom,
bAlign center, autoLineFeed true. `UI_sys_subtitle.uif`: text node (font `sbt-sys-M`) 1024 x 100 with the box
x -0.05..0.05, y 0..-0.1 of its size, a 1024 px wide box hanging 100 px below the node. With the generator offset the
box spans y 520..620 of the 1280 x 720 canvas; lines are left aligned inside a block centered in the box, the block
bottom on the box bottom, wrapped at 1024 px. A line keeps its trailing spaces in its width (many `.subp` pieces end
in one before a line break). The glyph size is the `sbt-sys-M` style's 22 x 22 with its spacing; the generator's 27 x 38
is not applied to this node.

Drawing: the subtitle text uses the shader `Draw2D_Border`: it samples the glyph cache at the pixel and at the pixel
plus and minus 1.2 times each screen derivative of the UV. With `c` the coverage at the pixel, `s` the sum of the four
taps and `n` the number of taps above 0: colour = `c`, alpha = 1 where `c > 0`, else `(s / n)^0.4`, both 0 where
`c + s < 0.0001`. The text is white whatever its node colour, the anti-aliased edge is dark grey, and the pixels beside
it are darkened: a black rim about one pixel wide at 1080p.

## UIF (UI model)

Little endian. Header:

| offset | type | field |
| --- | --- | --- |
| 0x00 | char[4] | `UIF ` |
| 0x04 | u16 | 0x0102 |
| 0x0A | u16 | node count |
| 0x0C | u16 | name count |
| 0x0E | u16 | texture count |
| 0x10 | u32 | node table offset |
| 0x14 | u32 | name table offset, relative to the data block |
| 0x18 | u32 | texture table offset (then one u32 0) |
| 0x1C | u32 | data block offset |

Names: u64 each (StrCode64 in the low 48 bits); node ids index them (name 0 is the scene, e.g. `UI_sys_option`).
Textures: {u32 length, u32 offset in the data block} to `/Assets/...ftex` paths. Node table: {u16 id, u16 type (0 root,
1 null, 2 mesh, 3 text), u32 offset}; parents come before children.

Node block (types 1 to 3, 0x50 bytes; root is 8 bytes with parent 0xFFFF):

| offset | type | field |
| --- | --- | --- |
| +0x00 | u16 | parent id |
| +0x04 | u16 | flags: 2 alpha blend, 4 additive, neither = blend off; 0x100 the rotation at +0x18 is a quaternion, otherwise Euler angles x, y, z in degrees; 1 palette color (name at +0x4E) |
| +0x06 | u16 | 0x11 meshes; text flags: bits 4-5 horizontal alignment (1 left, 2 center, 3 right), bits 7-8 vertical alignment (1 top, 2 center) |
| +0x08 | f32[2] | size: scales the node's own geometry only |
| +0x10 | f32[2] | scale (inherited) |
| +0x18 | f32[4] | rotation quaternion |
| +0x28 | f32[4] | translation, UI units |
| +0x38 | f32 | draw order: accumulated z + 10 |
| +0x3C | f32[4] | color RGBA (inherited multiplicatively) |
| +0x4C | i32 | -1 |

Mesh (+0x50, 0x38 bytes): u16 vertex count, u16 triangle count, u32 positions (data block, 16 bytes each, x y used),
u32 uvs (16 bytes each), i32 -1, u32 vertex remap (data block, u16 per vertex), u32 triangle indices (u16), u16 color
point count, u16 translate point count, u32 color point table, u32 translate point table (file offsets), u16 material
name (at +0x74; the `_s` animations address materials by this name), u16 4, u16 texture binding count, u16 parameter
count (30), u32 bindings (file offset: {u16 slot name, u16 texture index}), u32 parameters (file offset: {u16 0, u16
name, f32 value}), i32 -1.

Text (+0x50, 0x3C bytes): f32[4] box corner a, f32[4] box corner b (fractions of `size`: the box; the text flags place
the text inside it), f32[4] size and scale, u16 text name, u16 font name, u32 0, i32 -1.

Translate points: 0x18 bytes, {u16 name, u8 kind (1), u8 vertex count, i32 vertex list (data offset, u16 indices into the
remap), f32 x, y, z rest position}. Color points are 8 bytes, {u16 name, u8 kind (0), u8 count, i32 list}; no P.T. model
has them. The eight 8-vertex meshes of the option menu (gauge, lines, bars) have two translate points each, vertices 0
to 3 (left end) and 4 to 7 (right end).

Geometry: a vertex at `v * size` UI units; 1 unit = 10 px of the 1280 x 720 canvas. Material slots are `Base_Texture`,
`Layer_Texture`, `Mask_Texture`, `Screen_Texture`; parameters `UCenter_*`, `VCenter_*`, `UShift_*`, `VShift_*`,
`URepeat_*`, `VRepeat_*`, `Blend_*` for `BaseTex`, `LayerTex`, `MaskTex`, `ScreenTex`. They are the `Draw2D_Ui2`
constants: uv' = (uv - center) * repeat + center + shift; rgb = mix(base, layer, Blend_Layer) * color; alpha =
mix(base.a, layer.a, Blend_Layer) * mix(1, mask.g, Blend_Mask) * mix(1, screen.g, Blend_Screen) * color.a, where the
screen texture is sampled in screen space.

The UI pictures are wrap-addressed (ftex address mode 0x11) except the logos `fox_logo_clp_nmp`, `kjp_logo_clp_nmp` and
`konami_logo_clp_nmp` (clamp): their meshes are 1280 x 1280 with a repeat of 1.66, so the clamped edge color fills the
screen around the logo.

## UILB (layout)

Little endian; the UIGB graphs use the same container.

| offset | type | field |
| --- | --- | --- |
| 0x00 | char[4] | `UILB` |
| 0x04 | u32 | 1 |
| 0x08 | u16 | model count |
| 0x0A | u16 | animation count |
| 0x0C | u32 | 1 when a camera block is present |
| 0x10 | u16 | name count |
| 0x12 | u16 | string count |
| 0x14 | u32 | model records, 0x64 bytes each |
| 0x18 | u32 | animation records, 0x14 bytes each |
| 0x1C | u32 | camera block or -1 |
| 0x20 | u32 | -1 |
| 0x24 | u32 | data size |
| 0x28 | u32 | string table: {u32 length, u32 offset from the data base} |
| 0x2C | u32 | data base; the u64 names (StrCode64) follow the data |

Model record: u16 name (the model's StrCode64, equal to its UIF scene name), u16 string (UIF path), u32 1 (0 in the
logo layouts), identity transform and color 1, i32 -1, u32 0x1F, u32 data offset of the model's animation list (u16 name
indices), u16 animation count, u16 1. Animation record: u16 name, u16 main string (`.uia` path or 0xFFFF), u16 shader
string (the `_s` file with material parameters, or 0xFFFF), u16 0, i32 -1, i32 -1, f32 speed (1). `UI_sys_opt_setin`
plays `UI_sys_opt_setin.uia` and `UI_sys_opt_setin_s.uia` together.

Camera block of `UI_sys_option.uilb`: u32 0x15, f32 72, f32 near 0.05, f32 far 4000, f32 focal length 50, f32[3]
position (0, 0, 150), quaternion (0, 0, 0, 1). A 50 mm lens on 24 mm film at 150 units sees 72 units: 10 px per unit
on 720 lines. Node depth does not scale the menu (the camera projects orthographically, 72 being the view height,
likely). The subtitle, ending, logo and opening layouts have no camera block.

## UIA (animation)

The Fox motion format of the gani files (`motion.md`): u32 magic 0x0BFCA2D2, u32 header size 0x20, u32 size, u32 0,
then the node tree of 0x30-byte records. Under `ROOT` (0xEA72054A) and one more level, each animated node is named by
the low 32 bits of its UIF node name, or of a mesh's material name for material parameters. Units table: u32 unit
count, track count, 0x01000000, frame count, 5 (ticks per frame), u32 unit offsets; unit: u32 channel, u8 track count,
u8 flags (4 static), two pad bytes, tracks {i32 data offset from the track entry, u16 index, u8 kind (low nibble 1..4 =
1 to 4 floats), u8 bits (16 in all files)}. Keys use the demo stream codec (`fsm.md`): a first value, then (u8 frame
delta, value) pairs until the frame count; the values are IEEE halves scaled by 128 (1.0 is stored as 0x2000).

Channels (StrCode32 names): `TRANSLATE` 0xA34AEDBA (3 floats), `COLOR` 0x6318D107 (4), `SCALE` 0x8550FCEE (3), `ROTATE`
0x10DFD233 (3), and material parameters by name (`UShift_ScreenTex`, `Blend_LayerTex`, ...; 1 float).

Playback: linear between keys, the last key held, 60 frames per second of game time. Each channel applies on the
components of its mask property; a `COLOR` mask of alpha only sets the alpha. A node of the main file named after a
mesh with children named after its translate points animates vertices: the point's vertices move by `(value - rest)`
on the masked axes (the option menu's gauge fill widens this way).

## UIGB (UI graph)

Same container as UILB: u16 node count at 0x08, u16 name count at 0x10, u16 string count at 0x12 (the layout paths), u32
node table at 0x14, data size 0x24, string table 0x28, data base 0x2C, names after the data.

Nodes have a variable size: u16 class (name index), u16 node name (name index), u8 record size, u8 kind, u8, u8 input
count, u32 inputs (file offset of {u16 source node name, u16 pin or 0xFFFF, u16 pin name, u16 0}), u8 parameter block
count, u8 parameter block size, u16, u32 properties (file offset of {u16 value name, u8, u8, u32 data offset}), u32
parameter block (data offset), then -1 fields and, for events, a u32 flag.

| class | role | parameters |
| --- | --- | --- |
| 0xFD2D8ABA2A2A | entry | |
| 0x32563631D310 | follows the entry; input of every event and start node | |
| 0xF48D5D63E340 | start: its actions run when the graph starts | |
| 0xC36617182039 | event: fires on a text id | u64 id |
| 0x61BAC8085A1C | play an animation of the graph's layouts | property: animation name, with a u16 layout instance name at its data offset; block: f32[3] 0, f32 speed at +0x0C, u8 0, u8 1, u32 loop at +0x14 |
| 0x8317F8087C1A | set visible | property: layout name; block: u64 target (a UIF node name, the layout name or StrCode64("") for the whole layout), u32 on |

The demos drive these graphs with `DemoUiFunctor_Create` and `DemoUiFunctor_Start` events (`fsm.md`) and text ids. The
fake crash screen of gc_p02_080 picks one of six `bug_expression.uigb` events from a time stamp (xorshift32, modulo 6),
each showing one page of `UI_sys_bug.uif`; the seventh page, `sh_bug_3`, has a mesh that every event hides and none
shows, so the original never displays it.

## Draw priorities

`ShUiBootInit.lua` `SetDrawPriorityTable` (higher is in front): SUBTITLE 150/151, TEXT 160/161, TELOP/PAUSE 170..175
(PAUSE_BG 171, PAUSE_MENU 173), POPUP 180..183, GAME_FADE 190..192, STRONG_TELOP_PAUSE 200..209, STRONG_SUBTITLE 210,
FRONT_EFFECT 245, ERROR 251. Others: overlay sprite 100, subliminal string 128 and noise 129, normal fade 192 (170 after
`FadeCustomSetting(170, 255)` in `OnInit`), strong fade 250, demo UI graphs 181.
