# UI data and the 2D layer

What P.T. draws on top of the 3D scene and the files behind it: fonts (`.ffnt`), language files (`.lng`), subtitle entries (`.subp`, see also `audio.md`), UI models (`.uif`), layouts (`.uilb`), animations (`.uia`) and UI graphs (`.uigb`). Addresses are eboot.elf (US 01.00). The port implementation is `src/engine/ui/` (formats, 2D batch) and `src/game/ui/` (game UI). Tools: `tools/ffnt.py`, `tools/lng.py`, `tools/uif.py`.

## Files

| data | package | use |
| --- | --- | --- |
| `/Assets/sh/font/font_def_ltn.ffnt`, `font_def_jp.ffnt` | chunk1 | UI font type 0 (menus); jpn uses `_jp` |
| `/Assets/sh/font/LatinFont.ffnt`, `KanjiFont.ffnt` | chunk1 | UI font type 1, "movie font" (subtitles); jpn uses Kanji |
| `/Assets/sh/lang/ui/OPTIONS.lng#<lang>`, `SYSTEM.lng#<lang>` | `ui_default_lang.fpkd` | menu and system texts, lang = eng fra deu spa jpn ita por |
| `/Assets/sh/ui/GraphAsset/Common/data/common_art.fox2` | `ui_default_data.fpkd` | `common_fnt_palette` (menu font sizes), `common_col_palette` |
| `/Assets/sh/ui/common_subtitle.fox2` | `ui_default_data.fpkd` | `subtitles_fnt_palette` (`sbt-sys-M`, per language) |
| `/Assets/sh/ui/Subtitles/boot/subtitle_boot.fox2` | `resident.fpkd` | `SubtitlesGenerator` `Default` |
| `/Assets/sh/ui/ModelAsset/sys_subtitle/Scenes/UI_sys_subtitle.uif` | `resident.fpk` | subtitle text node |
| `/Assets/sh/ui/ModelAsset/sys_option/Scenes/UI_sys_option.uif`, `UI_sys_opt_*.uia`, `LayoutAsset/sys_option/UI_sys_option.uilb` | `ui_default_data.fpk` | option (pause) menu |
| `/Assets/sh/ui/Subtitles/subp/EngVoice/<Lang>Text/trial.subp` | `subtitle.fpk` per language | subtitles and puzzle captions |
| `/Assets/sh/effect/vfx_pic/text/text_sub01_alp` .. `text_sub10_alp`, `noise/Noise_00007`, `noise/Noise_00007_nrm`, `holl/holl_002_alp` | resident, texture.qar | subliminal service and peephole overlay |
| `/Assets/sh/ui/GraphAsset/opening.uigb`, `bug_expression.uigb`, layouts `sys_opening`, `sys_bug`, `sys_trophy` | `resident.fpk` | demo UI of the preface (gc_p02_500) and of gc_p02_080 |
| `/Assets/sh/ui/GraphAsset/promotion_ending.uigb`, layouts and models `sys_ending`, `sys_logo` | `ending.fpk` (graph and layouts also in `ui_default_data.fpk`) | ending narration, credits and logos |
| `/Assets/sh/ui/ModelAsset/*/Pictures/*` | texture.qar | all UI pictures, the ending's included |

Font slots (`Ui_SetLanguage` 0x94C760): slot 1 (2 for jpn) = `font_def_*`, slot 3 = `LatinFont`/`KanjiFont`; UI font type table: type 0 -> slot 1/2, type 1 -> slot 3 (`ShUiBootInit.lua` shows the same mapping for the Japanese build).

## FFNT

Little endian. Header: `FXFT`, u16 1, u8 entry count (2), u8 0, u16 10; entry table at 0x10 of {char[4] tag, u32 offset, u32 size}.

`GLYP` entry: 16-byte header, then u16-sorted glyph records of 20 bytes.

| offset | type | field |
| --- | --- | --- |
| +0x02 | u8 | em size in bitmap pixels (0x35 = 53 `font_def_*`, 0x36 = 54 Latin/Kanji) |
| +0x06 | u16 | glyph count |
| +0x08 | u32 | record bytes |
| +0x0C | u8 | pad: extra advance on each side of a glyph, font units (0 `font_def_*`, 8 Latin/Kanji) |

Glyph: u32 code point (low 28 bits), u16 x, u16 y, u8 width, u8 height, u8 layer (bit plane), u8 advance, u8 bearing (left), s8 top (from the line top), u16 flags (bit 0 and 1 each remove one pad from the advance), u32 0.

`FTDT` entry: u8 0, u8 log2 width (10), u8 log2 height (9), u8 8 (bit planes), u32 size, 8 zero bytes, then width x height bytes; glyph pixels are bit `1 << layer` of each byte (all P.T. glyphs use layer 0).

Runtime (0xD35890 measure, 0xD35EA0 line height, 0xD38710 glyph cache): a glyph is copied into a cache texture through a 3x3 kernel (1 2 1 / 2 4 2 / 1 2 1, /16, 0 or 255 input). The cache is one 2048 x 1024 R8 texture with a single mip level, sampled bilinear by `Draw2D` (`outColor = max(tex, mask) * color`, the menu_trace_rb draw trace; its cells hold the glyph box only, byte for byte what the kernel gives). 0xD36980 builds a glyph's UV rect half a texel past its cell on every side (`u - 0.5 / cacheWidth` to `u + du + 0.5 / cacheWidth` in normalized UV, likewise v) while the quad keeps the glyph's size (`fontWidth * w / em`), so a glyph shows w / (w + 1) of the scale its quad suggests (3 % smaller for the menu's 30 x 35 'O'). The port builds its atlas and UVs the same way. Advance = `fontWidth * (advance + 2 pad - flag pads) / em + textSpace` per character; line height = `fontHeight * (em + 2 pad) / em`, plus `lineSpace` between lines (0x10296F0 layout, 0x102AC80 run width). The glyph quad sits at `pen + fontWidth * (pad + bearing) / em`, `lineTop + fontHeight * (pad + top) / em`. Line breaks: LF, CR LF; a run wider than the box breaks at the last space, otherwise at the character. Unknown code points fall back (port: U+25A2, then `?`).

Font styles (`UiFontDataElement`: +0x30 language, +0x38 fontName, +0x70 fontWidth, +0x74 fontHeight, +0x78 textSpace, +0x7C lineSpace, +0x80 fontEdge; registrar 0x102B200):

| name | language | width x height | textSpace | lineSpace | fontEdge |
| --- | --- | --- | --- | --- | --- |
| `cmn-cmn-sys-M` | all | 20 x 20 | -1 | 0 | 0.25 0.13 0.28 1024 |
| `cmn-cmn-sys-L` | all | 24 x 24 | -1 | 0 | 0.25 0.13 0.28 1024 |
| `sbt-sys-M` | default | 22 x 22 | -6 | -4 | 0.25 0.18 0.3 120 |
| `sbt-sys-M` | jpn | 22 x 22 | -7 | -4 | 0.25 0.15 0.36 120 |

The `sbt` spacing (-6 at 22) cancels the 16-unit pad of the movie fonts, which is why subtitles use font type 1 and menus (-1) type 0. `fontEdge` reaches the font table (0x102E0D0 hands each element to 0xFDB4A0, which keeps it at +0x20 of the font record next to width, height and spacing; fonts registered without an element get 0.25 0.13 0.28 1024 from 0x148B230) and the text object returns it (vtable 0x1C09400 +0x110, 0x1007920, from the record at +0xD0), but no draw path reads it (likely: no call through +0x110 or the second base's +0x120 in the UI and 2D code at 0xC00000 to 0x1100000 reaches a text object, and no code there reads +0x20 of the record). The subtitles' dark rim is their shader's, `Draw2D_Border` (subtitle layout, below), which takes no edge parameters.

## LANG (.lng, version 2)

`LANG`, u32 version 2, char[4] `LE\0\0`, u32 count, u32 entry table, u32 key strings, u32 values, u32 0. Entry: {u32 key offset, u32 value offset}. Key: ASCII, zero terminated. Value: u16 color id (1 subtitle-like texts, 0x101 option texts, 0x303 system texts), then UTF-8 text, zero terminated. `OPTIONS.lng` also carries `TRIA1000_*` copies of the subtitle texts (unused by the port; the subtitle system reads `.subp`).

## SUBP entries

See `audio.md` for the index. Entry header (12 bytes; field names from Atvaark's SubpTool, meanings from 0x84B100 and the accessors of vtable 0x1BC12B0):

| offset | type | field |
| --- | --- | --- |
| +0x00 | u16 | 0x4C01 |
| +0x02 | u8 | line (timing) count |
| +0x03 | u8 | category: 5 in-game voice, 7 super (caption) |
| +0x04 | u16 | text bytes including the terminator |
| +0x06 | u16 | text bytes (+ additional length) |
| +0x08 | u16 | character id (2 caster, 0xF mib/bag, 0xD baby and ending voice, 1 captions) |
| +0x0A | u8 | range: index into the radius table 0x140BF70 {0, 20, 40, 70} m (0x84AB50); 0 and anything above 3 (0xFF) set no distance limit |
| +0x0B | u8 | 1 |

Timings are u16 pairs in 1/100 s (the accessors multiply by 10 to get ms). Text lines are separated by `$`; CR LF inside a line is a line break. Encoding: eng, jpn and por are UTF-8, fra, deu, spa and ita are Windows-1252 (the eboot converts per language in 0x858800; `SubtitleTable` returns the raw bytes, the port converts on display).

Category priorities (`PriorityTable.lua`, start and playing): 0 unknown 255, 1 game 0, 2 cutscene 0, 3 event header 10, 4 chapter header 20, 5 in-game 30, 6 summary 40, 7 super 0; the port shows the active subtitle with the lowest playing value (ties: newest).

Distance (0x8576B0, the visibility check of a playing voice subtitle, in the voice subtitle vtables at 0x1BC0EA8 and after): the parsed entry (0x84B100 copies header +0x03 category, +0x0A range and +0x08 character to +0x04, +0x05 and +0x06) gives the range; r = 0x84AB50(range) = table[range] squared, 0 for range 0 or above 3. With r = 0 the entry is shown at any distance. Otherwise the voice emitter position comes from the 48-slot table at 0x1C53910 (0x849BD0, 0x84EEB0; no slot hides the line); when a camera exists (0x7E08C0) and the subtitle system flag +0x16A is set, a source within half of the camera's +0x100 angle from its +0x50 direction scales r by camera +0xFC / 28; the line shows while the squared distance from the source to the listener at subtitle system +0x140 is at most r. Every P.T. voice entry has range 0 and every caption 0xFF (27 entries, English table), so no P.T. subtitle is hidden by distance. The radio lines are limited by area only: `trapSubtitleVisibleArea.lua` (`VisibleControlSubtitle`) shows them on ENTER and hides them on OUT of the f010 box at (-6.68, -0.45, 8.02), rotated -90 degrees about y, scale (5, 5, 17), and the f050 box of the same shape. Check (route walking from the f010 radio emitter at about (-8.3, 3.1, 26.2) back through the start room door): shown at 1.3, 5.1, 8.4 and 8.6 m, hidden from the first step out of the box on (9.6 and 12.1 m), with the log line `range 0 (no distance limit)` at the start of `tria1000_101010`.

Lifetime: a voice subtitle belongs to the sound whose marker started it. The playing object keeps that sound's playing id at +0x18 (the vtables at 0x1BC0EA0 and after; +0x60 is 0x857680, +0x68 the distance check above), and the subtitle system looks it up in the playing id table at 0x1C53910, which the sound object update 0x72CCF0 fills right after its post (0x848230 -> 0x84EE20: id, emitter position, flag). The port keeps the playing id the marker came with (`SoundSystem` marker callback, `Game::QueueSubtitle`) and ends the subtitle when that id is no longer playing (`SubtitlePlayer::EndStopped`, log `ended with its sound`): a stop trap, a stage unload, or `Set_state_game_over`'s Stop_ALL after Lisa's kill. Before this, the radio's subtitle kept its own 90 s timeline through the game over and the wake-up in the start room, where the area that had shown it was still in effect. Test: walkthrough route `tests/walkthrough/radio_gameover.txt` (scenario `radio_gameover`).

Visibility (`Options_ApplySubtitle` 0x91D550): the option sets daemon +0x169 and force display (on) or force non-display (off) for all 64 categories, so captions follow the subtitle option like voices. `0x847F20(on)` (steps 8 and 27 on, 11 and 29 off; `ScreenEffects::subtitles_enabled` in the port) raises the subtitle UI draw priority to 0xD2 (210, `__STRONG_SUBTITLE`, above fades) and restores it after (0x853C10).

## Captions (terop)

The puzzle captions are subtitle entries of category 7 played by message id through the text system (`GameCore+0xB0` vfunc +0x10, generator `Default`):

| message id | entry | English text | time |
| --- | --- | --- | --- |
| 0x4AF4F3D4 | `tria1000_1k1010` | Forgive me, Lisa / There's a monster inside of me | 0 to 3.00 s |
| 0x4FA7C4BD | `tria1000_1l1010` | I can hear them calling to me from hell | 0 to 3.00 s |
| 0xF124B2D3 | `tria1000_1m1010` | No turning back now | 0 to 2.00 s |
| 0xCE86DEBA | `tria1000_1j1010` | My voice, can you hear it? / This sign, can you read it? / I'll wait forever if you'll just come to me. | 0 to 5.00 s |
| 0x7D33E7DF | `tria1000_141010` | the ending voice (9 lines, 3.70 to 38.75 s) | driven by the demo time (step 28) |

The ids are the low 32 bits of StrCode64 of the lowercase entry id, like every `.subp` key.

## Subtitle layout

`SubtitlesGenerator` (registrar 0x84C880; +0x90 key, +0xA0 color, +0xB0 offset, +0xC0 size, +0xD0 fontSpace, +0xD4 lineSpace, +0xD8 hAlign, +0xDC vAlign, +0xE0 bAlign, +0xE8 fontName, +0xF0 autoLineFeed; defaults color 1, size 32 x 32, hAlign center, vAlign top, bAlign center). P.T. data: color 1 1 1 1, offset (0, -16), size (27, 38), spaces 0, hAlign 0 TEXT_LEFT, vAlign 2 VERTICAL_BOTTOM, bAlign 1 BOX_CENTER, autoLineFeed true. The subtitle graph node copies these into its UI (0x854A30).

`UI_sys_subtitle.uif`: text node (font `sbt-sys-M`) 1024 x 100 with the box x -0.05..0.05, y 0..-0.1 of its size, i.e. a 1024 px wide box hanging 100 px below the node. With the generator offset the box spans y 520..620 of the 1280 x 720 canvas; lines are left aligned inside a block centered in the box, the block bottom on the box bottom, wrapped at 1024 px. A line keeps its trailing spaces in its width: 42 `.subp` pieces end in a space and 118 of the 182 CR LF breaks have one before them (`investigate \r\nthe`; English 16 of 23), and in radio_subtitles_rb those subtitles sit half a space (5 px at 1080p) further left than they would without it; with the spaces kept the port's rows of frames 2320, 3220 and 3720 cover the capture's columns and rows exactly (625..1294 and 624..1202, 526..1380 and 526..1391, 513..1414).

Drawing: the subtitle nodes give their text the technique `Draw2D_Border` (0x854A30 for `EvSubtitlesNode` and 0x855800 for `EvControlSubtitlesNode` pass its StrCode, made by the static initialisers 0x854F40 and 0x855D40, to the text object's vfunc +0x98; radio_subtitles_rb, the first capture with subtitles on, compiles its pixel shader 1437ef5a6c2ac93b and no other). The shader (`dump/shaders/GrSystemShaders_ps4/Draw2D_Border`) samples the glyph cache at the pixel and at the pixel plus and minus 1.2 times each screen derivative of the UV, each offset clamped to `m_localParam[2]` = (min u, min v, max u, max v), which the text draw 0xDDAFB0 sets (constant 0xB5) to 4 texels either way (4 x 0.5 / size, doubled). With `c` the coverage at the pixel, `s` the sum of the four taps and `n` the number of taps above 0: colour = `c`, alpha = 1 where `c > 0`, else `(s / n)^0.4`, both 0 where `c + s < 0.0001`. COLOR0 is not read, so the text is white whatever its node colour. Every pixel the glyph touches is opaque, its grey the coverage, so the anti-aliased edge is dark grey; the pixels beside it are darkened by up to 100 %: a black rim about one pixel wide at 1080p. Against radio_subtitles_rb frame 3720 the port's line differs by 13.9 on average over the text pixels (0.25 px of text offset in any direction gives 23 to 28); the capture's strokes are a little softer (fit capture = 0.84 port + 25, cores 6 levels darker), not explained by the film grain over them.

Glyph size: `SubtitlesBasic.uigb` wires the generator node (class 0x34B4A8FF76AB) into a text setting node (class 0x75AB8FF1BBEA, apply 0xFEA7E0) through the pins `text`, `color`, `size`, `fontSpace`, `lineSpace`, `textAlign`, `verticalAlign`, `fontName`, `autoLineFeed` and `boxAlign`. 0xFEA7E0 calls the text object's size setter (vfunc +0x130 with size x and y) and spacing setter (+0x140) only when bit 0 of the text object's byte +0x81 is set, and the three alignment setters only when it is clear. The port keeps the `sbt-sys-M` style size, 22 x 22 per em with its spacing (-6, -4), and takes the alignments from the generator (likely: the bit is clear for the subtitle node). At the generator's 27 x 38 the glyphs were taller than the font's own proportions (reported against the PS4 version) and the widest authored English row, "The day of the crime, ... retrieved the rifle,", measured 1040 px and wrapped in the 1024 px box; at 22 x 22 it measures 756 px. Measured on 1080p renders: 'o' 19 x 29 px (height / width 1.53) at 27 x 38, 16 x 17 px (1.06) at 22 x 22; the font's own 'o' is 26 x 28 font pixels (1.08).

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

Names: u64 each (StrCode64 in the low 48 bits); node ids index them (name 0 is the scene, e.g. `UI_sys_option`). Textures: {u32 length, u32 offset in the data block} to `/Assets/...ftex` paths. Node table: {u16 id, u16 type (0 root, 1 null, 2 mesh, 3 text), u32 offset}; parents come before children.

Node block (types 1 to 3, 0x50 bytes; root is 8 bytes with parent 0xFFFF):

| offset | type | field |
| --- | --- | --- |
| +0x00 | u16 | parent id |
| +0x04 | u16 | flags (0xFF93D0, 0xFFBB70): 2 alpha blend, 4 additive, neither = blend off; 0x100 the rotation at +0x18 is a quaternion, otherwise Euler angles x, y, z in degrees; 1 palette color (name at +0x4E; 0x200 and 0x400 then clear bits 3 and 4 of the node's inheritance mask at +0x54). Every node is created visible (0x100CB70 calls 0xFFA660(node, 1)) |
| +0x06 | u16 | 0x11 meshes; text flags: bits 4-5 horizontal alignment (1 left, 2 center, 3 right), bits 7-8 vertical alignment (1 top, 2 center), low bits 3 or 1 (0x0E13 subtitle, 0x113, 0x93, 0x123, 0x111) |
| +0x08 | f32[2] | size: scales the node's own geometry only |
| +0x10 | f32[2] | scale (inherited) |
| +0x18 | f32[4] | rotation quaternion |
| +0x28 | f32[4] | translation, UI units |
| +0x38 | f32 | draw order: accumulated z + 10 |
| +0x3C | f32[4] | color RGBA (inherited multiplicatively; `UiInheritanceSetting`) |
| +0x4C | i32 | -1 |

Mesh (+0x50, 0x38 bytes): u16 vertex count, u16 triangle count, u32 positions (data block, 16 bytes each, x y used), u32 uvs (16 bytes each), i32 -1, u32 vertex remap (data block, u16 per vertex), u32 triangle indices (u16), u16 color point count, u16 translate point count, u32 color point table, u32 translate point table (file offsets), u16 material name (at +0x74; the `_s` animations address materials by this name), u16 4, u16 texture binding count, u16 parameter count (30), u32 bindings (file offset: {u16 slot name, u16 texture index}), u32 parameters (file offset: {u16 0, u16 name, f32 value}), i32 -1.

Text (+0x50, 0x3C bytes): f32[4] box corner a, f32[4] box corner b (fractions of `size`: the box; the text flags place the text inside it, e.g. `op_camera_y_axis` (0x113) is left aligned in a centered 400-unit box and `op_britness_tip` (0x93) sits at the top of its 150-unit box, matching the original screen), f32[4] size and scale, u16 text name, u16 font name, u32 0, i32 -1.

Translate points (0xFFC280): 0x18 bytes, {u16 name, u8 kind (1), u8 vertex count, i32 vertex list (data offset, u16 indices into the remap), f32 x, y, z rest position}. Color points are 8 bytes, {u16 name, u8 kind (0), u8 count, i32 list}; no P.T. model has them. The eight 8-vertex meshes of the option menu (gauge 50 and 54, lines 74, 109 and 146, bars 81, 96 and 137) have two translate points each, vertices 0 to 3 (left end) and 4 to 7 (right end).

Texture address mode: ftex +0x12, one nibble per axis, 0 clamp and 1 wrap (`textures.md`). The UI pictures are wrap (0x11) except the logos `fox_logo_clp_nmp`, `kjp_logo_clp_nmp` and `konami_logo_clp_nmp` (0): their meshes are 1280 x 1280 with a repeat of 1.66, so the clamped edge color fills the screen around the logo.

Geometry: a vertex at `v * size` UI units; 1 unit = 10 px of the 1280 x 720 canvas at z = 0 (the layout camera, below). Material slots are `Base_Texture`, `Layer_Texture`, `Mask_Texture`, `Screen_Texture`; parameters `UCenter_*`, `VCenter_*`, `UShift_*`, `VShift_*`, `URepeat_*`, `VRepeat_*`, `Blend_*` for `BaseTex`, `LayerTex`, `MaskTex`, `ScreenTex` (two more names, value 1.0 and 1200.0, unresolved). They are the `Draw2D_Ui2` constants: uv' = (uv - center) * repeat + center + shift; rgb = mix(base, layer, Blend_Layer) * color; alpha = mix(base.a, layer.a, Blend_Layer) * mix(1, mask.g, Blend_Mask) * mix(1, screen.g, Blend_Screen) * color.a, where the screen texture is sampled in screen space (likely pixel / screen height; the port uses that).

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

Model record: u16 name (the model's StrCode64, equal to its UIF scene name), u16 string (UIF path), u32 1 (0 in the logo layouts), identity transform and color 1, i32 -1, u32 0x1F, u32 data offset of the model's animation list (u16 name indices), u16 animation count, u16 1. Animation record: u16 name, u16 main string (`.uia` path or 0xFFFF), u16 shader string (the `_s` file with material parameters, or 0xFFFF), u16 0, i32 -1, i32 -1, f32 speed (1). `UI_sys_opt_setin` plays `UI_sys_opt_setin.uia` and `UI_sys_opt_setin_s.uia` together; `UI_sys_opt_lp_1` and `UI_sys_end_logo_lp` have only their `_s` file. `UI_sys_end_logo_setin_s.uia` and `_setout_s.uia` are in the ending package, but no layout references them.

Camera block of `UI_sys_option.uilb`: u32 0x15, f32 72, f32 near 0.05, f32 far 4000, f32 focal length 50, f32[3] position (0, 0, 150), quaternion (0, 0, 0, 1). A 50 mm lens on 24 mm film at 150 units sees 72 units: 10 px per unit on 720 lines. Node depth does not scale the menu: setin moves node 1 from z 10 to 0 and setout from 0 to 9.3, yet menu_detail_rb shows the gauge (x 226 to 552) and the title box ((167, 109) to (288, 131)) at their final size from the first setin frame (1104) on and through setout (1374 to 1377) (likely: the camera projects orthographically, 72 being the view height). The port draws the option menu without depth scaling. The subtitle, ending, logo and opening layouts have no camera block.

## UIA (animation)

The Fox motion format of the gani files in the motion archives (`motion.md`): u32 magic 0x0BFCA2D2, u32 header size 0x20, u32 size, u32 0, then the node tree of 0x30-byte records (u32 name hash, u32 name offset, u32 has units, i32 data offset, u32 data size, i32 parent, child, previous, next, u32 extra size, two u32). Under `ROOT` (0xEA72054A) and one more level, each animated node is named by the low 32 bits of its UIF node name, or of a mesh's material name for material parameters. Units table: u32 unit count, track count, 0x01000000, frame count, 5 (ticks per frame), u32 unit offsets; unit: u32 channel, u8 track count, u8 flags (4 static), two pad bytes, tracks {i32 data offset from the track entry, u16 index, u8 kind (low nibble 1..4 = 1 to 4 floats), u8 bits (16 in all files)}. Keys use the demo stream codec (`fsm.md`): a first value, then (u8 frame delta, value) pairs until the frame count; the values are IEEE halves scaled by 128 (1.0 is stored as 0x2000).

Channels (StrCode32 names): `TRANSLATE` 0xA34AEDBA (3 floats), `COLOR` 0x6318D107 (4), `SCALE` 0x8550FCEE (3), `ROTATE` 0x10DFD233 (3, only in `get_chip`, all zero), and material parameters by name (`UShift_ScreenTex`, `Blend_LayerTex`, ...; 1 float).

Playback: linear between keys, the last key held. The timeline runs at 60 frames per second of game time, not one frame per game update (the game updates at 30 Hz): only that rate fits the ending, where `crdt_setin_short` (239 frames) is restarted 306 demo frames later and the fade out at the end of `end_logo_setin` (frames 433 to 532) completes just after the `setout` event 475 demo frames in (likely). Values replace the node's, except `COLOR`: the animated alpha replaces the node alpha and the animated RGB multiplies the node RGB (inferred: the `UI_sys_end_credit_all` pictures are black with alpha 0 in the UIF and the credit animations key (1, 1, 1, 1) on them, while the staff names must be dark on the off-white ending fade).

Application (0x100AF70 nodes, 0x100B300 points): each channel is applied only on the components of its mask property (`TRANSLATE` 0xCD8A76D1, `SCALE` 0x2BE445E4, `ROTATE` 0x2B94BE26, `COLOR` 0x931756B1); without a mask the value replaces the whole vector (0xFFA5A0, 0xFFA5C0, 0xFFA5D0), a `COLOR` mask of alpha only sets the alpha (0xFFA640). The port does not read the masks; its `COLOR` rule above gives the same result for every P.T. key (RGB keyed at 1). A node of the main file that is named after a mesh and has children named after its translate points animates vertices: 0xFFD530 moves the point's vertices by `(value - rest)` on the masked axes. `UI_sys_option_bgrh_scle` has one such track, point 0xC0F0B311 of the gauge fill (node 50, 0x38E510A4): x from -0.2995 at frame 0 to 0.329 at frame 99, which widens the fill from its left end to the full gauge.

## UIGB (UI graph)

Same container as UILB: u16 node count at 0x08, u16 name count at 0x10, u16 string count at 0x12 (the layout paths), u32 node table at 0x14, data size 0x24, string table 0x28, data base 0x2C, names after the data.

Nodes have a variable size: u16 class (name index; the StrCode64 registered with a factory by 0xFD9510), u16 node name (name index), u8 record size, u8 kind, u8, u8 input count, u32 inputs (file offset of {u16 source node name, u16 pin or 0xFFFF, u16 pin name, u16 0}), u8 parameter block count, u8 parameter block size, u16, u32 properties (file offset of {u16 value name, u8, u8, u32 data offset}), u32 parameter block (data offset), then -1 fields and, for events, a u32 flag.

| class | role (from the data) | parameters |
| --- | --- | --- |
| 0xFD2D8ABA2A2A | entry | |
| 0x32563631D310 | follows the entry; input of every event and start node | |
| 0xF48D5D63E340 | start: its actions run when the graph starts | |
| 0xC36617182039 | event: fires on a text id | u64 id |
| 0x61BAC8085A1C | play an animation of the graph's layouts | property: animation name, with a u16 layout instance name at its data offset; block: f32[3] 0, f32 speed at +0x0C, u8 0, u8 1, u32 loop at +0x14 |
| 0x8317F8087C1A | set visible | property: layout name; block: u64 target (a UIF node name, the layout name or StrCode64("") for the whole layout), u32 on |

## Demo UI

Demo ExecCommand functors (`demo.md`): `DemoUiFunctor_Create` (0x77C290: graph name 0x0C8D2F3D, uiFile 0xEBD4EBE2 through the DemoData `fileParams`, int 0xFEB36856 indexing the table at 0x14017C0 {15, 2}; entry 15 of the `ShUiBootInit` priority table is POPUP_BG 181 and entry 2 UNDER_SUBTITLE_BG 145, likely; every P.T. demo passes 0), `DemoUiFunctor_Start` (0x77BB40), text 0xECEC4D259E21 (0x77BDB0: sends text id 0xE5B44417 to the graph; id 0x90BA012E9D3E is replaced by one of six bug events chosen by xorshift32 of a time stamp modulo 6, below).

The f120 fake crash's bug screen (confirmed from 0x77BDB0 and the data): gc_p02_080 sends the text 0x90BA012E9D3E at frame 5401, and 0x77BDB0 reads `sceKernelReadTsc` (0x42C9C0) at that moment, takes the low 32 bits through one xorshift32 step (13, 7, 5) and switches on the result modulo 6. Each case is an event of `bug_expression.uigb` that plays the setin animation and shows one mesh of `UI_sys_bug.uif` (each mesh one 2048 x 2048 picture) and hides the others; 0xE77E6C72FA82, sent at 5876, plays the setout:

| case | event | picture | page |
| --- | --- | --- | --- |
| 0 | 0x119CABD7B66A | `sh_bug_1` | grey, "Development halted due to inexplicable bug." in seven languages, turned upside down |
| 1 | 0x69D97AC6AF45 | `sh_bug_2` | black, "Fix this damn bug (cause = ??) before release!" |
| 2 | 0xC19FC491E18F | `sh_bug_4` | white, "Knowing you, I was sure you'd notice this game and play it. ... Contact me. - J." |
| 3 | 0xF89D156E5DA1 | `sh_bug_5` | red, "I'm heading there now." repeated over the page |
| 4 | 0xED7A96A7C6B6 | `sh_bug_6` | yellow, "I'll call later." repeated over the page |
| 5 | 0xA957904C892D | `sh_bug_7` | black, "This game is purely fictitious. It cannot harm you in any way, shape, or form." |
| 6 (port only) | none | `sh_bug_3` | yellow, blue text: "Release the game for free, and find someone who matches the suspect's profile from among those who downloaded it." |

In the original `sh_bug_3` (the case 6 row) has a mesh (0x8F1B33D1D824) that every event hides and none shows, so it never appears. All pages carry the same lines in English, French, German, Spanish, Japanese, Italian and Portuguese. The time stamp makes each fake crash an even draw of the six, independent of the boot and of the earlier ones; the captures show case 4 (floor_f120 3900 to 4130). The port reads its stand-in for the counter (`ReadTsc`, `src/game/game_objects.cpp`: the high resolution clock, or with `--seed` a fixed sequence) at the same moment (`DemoUi::Text`) and logs `ui: bug screen <case> of 7`; before, a fixed seeded generator gave every first fake crash after a start case 5.

Port addition (not the original's behavior): at the user's request the port shows `sh_bug_3` too, as case 6. It takes the same draw modulo 7, so each of the seven pages comes with a chance of 1/7 (the original's six 1/6 each), and builds the missing event from case 0's: the setin animation and the seven visibility actions, with sh_bug_3's mesh the one shown; the setout at 5876 hides all seven meshes, sh_bug_3's included. `--bug-screen 0..6` sets the case (comparisons with a capture; 6 is the addition).

| demo | graph | layouts | events (demo frames) |
| --- | --- | --- | --- |
| gc_p02_500 (preface) | `opening.uigb` | `UI_sys_company_logo`, `UI_sys_opening_terop` | 30 7780s Studio logo in, 210 out, 300 "Watch out. The gap in the door..." in, 890 out |
| gc_p02_080 | `bug_expression.uigb` | `UI_sys_bug`, `UI_sys_trophy` | 5401 one of six bug pictures in, 5876 out |
| gc_p06_010_final | `promotion_ending.uigb`, a second instance from 8549 | credit, credit_all, fox, kjp and konami logos, narration, right, logo | narration pictures 223 to 2325 (the lines of caption 0x7D33E7DF), HIDEO KOJIMA 3864, GUILLERMO DEL TORO 4652, NORMAN REEDUS 5784, IN 6090, SILENT HILL(S) 6447 to 6922, staff 7070 to 7610, Kojima Productions 7695, Fox Engine 7905, Wwise 8115, teaser note 8325, Konami 8565 |

The preface pictures are single quads with one BC3 2048 x 1024 texture each (`UI_sys_logo_company.uif` node 2 `sh_logo_company_alp`, `UI_sys_opening_terop.uif` node 2 `sh_terop_op_01_alp`, every text line white at alpha 255); their layouts only fade node 1 (`setin`/`setout`, 19 frames). The captures (explore_boot, explore_boot_rb) show only texture rows 0 to 511 of both (the first MiB of mip 0): "STUDIO" and the second paragraph ("The only me is me...") stay near black and the "7780s" digits are cut at 1080p row 540 (texture row 512); shadPS4 keeps only the first of the 1 MiB slices the game streams mip 0 in, so the original shows both paragraphs alike. Where the capture is intact the port matches it at neutral output gamma (first paragraph region mean 23.7 against 23.7, logo region 68.1 against 67.2). Timing (explore_boot_rb, OPTIONS pressed at 2426): the text block starts fading out at 2880.1, so the preface clock started at 2435.1; the port starts it at 2430.5 and fades the text out from 2875.5 (the close press releases step 7 at once, gameplay.md 14.4, then one controller step to step 8 and the 3 start frames of `demo.md` Playback); against the capture's reactions to buttons, 3 frames late (Option menu notes below), that is 1.6 frames early.

A graph shows nothing before its start (likely): `DemoUiFunctor_Create` builds the instance (0x7A0D10, 0x7A0AD0, 0xFDCF10) and `DemoUiFunctor_Start` runs it (0x7A0F30, 0xFDD8A0, which sends the start event to every node), and the start node's actions hide what is not to be seen yet, among them the ending's three logo layouts (`UI_fox_logo`, `UI_kjp_logo`, `UI_konami_logo`), opaque 1280 x 1280 pictures at alpha 1 in their UIF. The ending creates both of its instances 10 frames before it starts them (frames 30 and 40, 8549 and 8559). The port drew a graph from its creation, so the KONAMI logo, drawn last of the three, covered the screen for those 10 frames: the black at the start of the ending and the grey before the logo's own fade-in at 8565 (the user's session at 30:38.25 and 33:00.38). No capture frame falls inside those windows: ending_street's pose log puts demo frame 2 at 1468, so frames 30 to 40 are 1482 to 1487, between its timeline frames 1480 and 1490, both black.

All ending texts are pictures. The narration model brings its own black background; the names in white use the demo's black fades; from frame 6250 the demo fades to (0.93, 0.94, 0.93) and the logo, staff names and teaser note are dark on that color (the Wwise and teaser models add `sh_bg_gray`). This off-white fade is the flat grey (about 239) frame of the ending.

## Save and load icon

`UI_sys_loading.uilb` places `UI_sys_loadicon.uif` (three circles, `sys_loadicon_ciecle_1_alp` and `_2_alp`) at (54.39, -26.66) units, low on the right (model record +0x2C; every node of the model sits at the origin), and lists `UI_sys_loadicon_setin` (node 1's alpha 0 to 1 over 29 frames), `UI_sys_loadicon_lp` with `_lp_s` (59 frames, looped: node 2's alpha 0.7 to 0 and the circles' material shifts, the blink) and `UI_sys_loadicon_setout` (node 1's alpha 1 to 0). The UI common data creates it at STRONG_PAUSE_ICON, priority 209 (0x949930: layout name StrCode64 `SaveLoadIcon`, category hash 0x95DA8A51F73). Its component (0x1284B50, vtable 0x1BD42C0; init 0x1284E00 finds the layout, the model and the three animations by name) starts when the save job posts SaveUiDisp or LoadUiDisp (0x1284F40): its two text nodes get the text of `Saving` or `Loading`, keys the language files do not have (theirs are `sys_saving` and `sys_loading`), so only the circles show, and setin and the loop play. Its update (0x1285030) adds the frame time and, once the job reports the write or the read done and more than 2.0 s have passed, plays setout; when setout has played it tells the job and hides. Every save shows it: the floor saves of the doors (floor_f070 2970 to about 3035), the options save when the pause menu closes (menu_detail_rb 1374 to past 1420), the first boot's save (preface_detail_rb 2440 to 2504), and the boot load (explore_boot_rb 180).

The port's `SaveIcon` (`src/game/ui/save_icon.cpp`) plays the same files at the same place and priority. `Game::RequestSave` and `RequestLoad` count every save and load (also with `--no-save`); the icon starts three 30 Hz frames after the request, as the job collects in the frame after a request, writes and posts SaveUiDisp in the next, and the icon takes the message in the third; the port's saves are done at once, so the 2 s rule alone ends it. Against menu_detail_rb the rise to 97 (capture 99) and the loop's flash to 167 (176) come 3 frames before the capture's, as every input there does (Port differences, input latency).

## Draw priorities

`ShUiBootInit.lua` `SetDrawPriorityTable` (higher is in front): SUBTITLE 150/151, TEXT 160/161, TELOP/PAUSE 170..175 (PAUSE_BG 171, PAUSE_MENU 173), POPUP 180..183, GAME_FADE 190..192, STRONG_TELOP_PAUSE 200..209, STRONG_SUBTITLE 210, FRONT_EFFECT 245, ERROR 251. Others: overlay sprite 100 (0x93A4A0), subliminal string 128 and noise 129 (0x93A810), normal fade 192 (170 after `FadeCustomSetting(170, 255)` in `OnInit`), strong fade 250, demo UI graphs 181. The port draws in this order: overlay, subliminal string, subliminal noise, subtitles (151 or 210), fade (its priority), option menu (171), demo UI graphs (181).

## Subliminal service

System table +0x50 (0x1C91C48); system object 0x93A670; trigger 0x93C150, update 0x93B380; draw passes set up in 0x93AE30 as full-screen quads.

- Trigger (index < 10): noise A = {in 0.03, hold 0.001, out 0.03}; unless `noString`, string = {in 0.03, hold 3.0, out 0.03}, position = two Fox RNG floats in [0, 1), texture `text_sub<index+1>_alp`; the flag arms burst B.
- Update (skipped when the frame delta is 0): each envelope advances one phase per frame (in: t = min(dur, t + dt), v = t / dur, ends at 1; hold: ends when t > dur; out: v = 1 - t / dur, ends at 0). End of A hold with the flag: B = {0.01, 0, 0.01}. End of the string hold: A restarts {0.03, 0, 0.03}. Noise phase += 0.7 per frame, wrapped to [0, 1]. The game updates at 30 Hz (`gameplay.md`), so a burst lasts three frames (in, hold, out); the port steps the service at 30 Hz.
- `Draw2D_ShSubliminalString`: uv = position + quad uv on `text_sub*` (wrap), color 0.1 x white, alpha = string value x texture alpha: dark handwriting at a random, wrapped offset.
- `Draw2D_ShSubliminalNoise`: normal = `Noise_00007_nrm`(phase + u, v); screen uv = (u - 0.001 + 0.002 normal.a, v - 0.03 + 0.06 normal.g) clamped; out.r = (B x 0.4 + mix(screen, noise, 0.1)).r, g and b = mix(screen, noise, 0.1), alpha = A (likely a 512 x 512 capture of the screen, 0x93AE30 sets up a 0x200 x 0x200 target and passes (512, 512, 1/w, 1/h) as parameter 0).
- Overlay +0x18(texture): a 2D sprite at priority 100, position (0, 0), size (1, 1), uv (0, 0, 1, 1), color 1 (full screen); +0x20 removes it. The peephole uses `holl_002_alp` (black, alpha 255 outside an irregular hole).

## Option menu

Pause menu object `ShPauseMenu` (0x9203E0, `notes_stage_ui.txt` 3.1); option UI component 0x1285340 (create), 0x12856B0 (bind nodes and animations), 0x12866C0 (texts), 0x1285F80 (update).

Text nodes (0x12856B0/0x12866C0): 150 `op_options`, 115 `op_back`, 41 `op_brightness_setting`, 48 `op_britness_tip`, 119 `op_subtitles_setting`, 121 .. 135 (every second id) `op_sub_none`, `_english`, `_french`, `_german`, `_spanish`, `_japanese`, `_itlian`, `_portuguese`, 78 `op_camera_setting`, 93 `op_camera_y_axis` with 88 `op_camera_normal` and 86 `op_camera_reversed` (row A, node 79), 108 `op_camera_x_axis` with 103 and 101 (row B, node 94). Brightness images: 61 and 69 at 0.09, 72 at 0.01: vfunc +0x128 of the UI system (vtable 0x1BC5F20, 0x122FCD0) calls 0xFFA640, which sets the node's color alpha. The gauge (50 fill, 54 background) is visible from the start and `bgrh_scle` is held at brightness / 10 through vfunc +0x98 of the option owner (0x12856B0 end, 0x1285F80 on change).

Mesh rendering (0x100CB70, 0xFFBB70, 0xFFC690, 0xFFCBB0, 0xFFD530, 0x100D020, 0x100D500): a mesh gets a vertex buffer of 16-byte vertices (half4 position, RGBA8 color from the constant 1.0, half2 UV) at its first draw (0xFFC690). A mesh that one of the model's animations moves by translate points (0x100A5F0: the animation's `0xB913232E` group has a node named after the mesh with children; in the option menu only the gauge fill 50) is flagged +0x80 bit 0x2000 and gets its buffer at creation; the point track (0x100B300, TRANSLATE value and mask 0xCD8A76D1) calls vfunc +0x40 = 0xFFD530, which rewrites the point's vertices as base + (value - rest) x mask. Drawable nodes are kept in a list sorted by the node priority (UIF +0x38, ascending, ties in insertion order: 0x100D020) and drawn in that order (0x100D500); render state 0x1B is the blend mode (UIF flag 2 -> 1, 4 -> 2). The menu's shader log (explore_boot_rb) shows `Draw2D_Ui2_BL` as the only `Draw2D_Ui2` variant, whose math is that of `Draw2D_Ui2` above. Textures are filtered trilinear without anisotropy (likely: FTEX filter 2 maps to the bilinear, linear mip entry of 0x1049D50 as for model textures, `rendering.md` section 3; the gauge edge in the first menu frame, column 400, rises 9, 133, 212 over the 215 plateau, which is LOD 2.8 trilinear over its 128 rows in 18 px, and the fill's end ramps 192, 149, 103, 62 where the port now gives 198, 150, 106, 61 with the capture's noise model below; 16x anisotropic filtering gave 18, 213, 255).

Brightness output: 0x952030 takes `0x13CEA30[level]` to renderer vfunc +0x88 (0xD8D050), which keeps `1 / value`; the frame end 0xD8A670 hands it to 0x103F890, which calls `sceVideoOutColorSettingsSetGamma` and `sceVideoOutAdjustColor` when it changes. No shader reads it: the video output applies it to the whole image. shadPS4 applies that gamma in its post process pass, after the point where the capture harness reads the frame, so reference captures are taken at neutral gamma.

Captures (brightness 5): the first menu frame of explore_boot (1900, readback off) shows the gauge at half (rows 624 to 635 of 1080, x 225 to 853, fill edge at x 550, fill 215 over the background 47), the three header lines (Brightness, Subtitles, Camera, core 25) and the selection bars (core 53). From 1920 on the lines are gone and the bars at 0.61x in every set, the gauge is gone in explore_boot, and in the readback sets (explore_boot_rb, radio_subtitles_rb) the gauge is a faint striped line (peak 47) from the first frame on. These are shadPS4 texture streaming faults, not the original's look (confirmed as far as the captures go):
- The affected textures keep their small mips in the first `.ftexs` stream (index file 1): `ui_sys_opt_gage` (128 x 128), `ui_sys_opt_line` and `ui_sys_opt_crs` (256 x 64) have all their mips there, `ui_noise_512` its mips 2 to 7. The stored mips are clean downsamples (the noise mips average 0.46 at every level); the shadPS4 fix of the reference worker (mip 0 streamed in 1 MiB slices into separate images, only the first kept) addresses the same streaming path.
- The gauge and lines sample their textures at LOD 2.8 (128 texel rows over 18 px) and vanish or turn to stripes; the stripes change with the texture u at the mesh's segment boundaries, not with the screen. The bars (LOD 0.4) keep the mip 0 share of the trilinear filter (0.58 expected, 0.61 measured).
- Everything that samples the screen noise at LOD 2.2 (repeat 10: bars, gauge, lines) is smooth in all captures (3x3 high-pass std 0.4 where the data gives 2.7) and sits at the `1 - Blend_ScreenTex` floor, while the icon glows (repeat 5, mostly mip 1) keep per-pixel noise that changes with the `lp_1_s` shifts. Rendering the port with only `ui_noise_512` mips 2 and below set to black reproduces the first capture frame within a few levels (icons p99 228 against 221, bar core 49.6 against 53.1, gauge fill 125 against 124 and background 24 against 28 (region means), selected texts 240 against 235); with the texture intact the icons and the gauge fill saturate at 255 and the bar core is 72.
- No code or data hides these nodes: 0x1285F80 touches only the text nodes, the brightness images and the animations, and of the layout's 20 animations only `lp_1_s`/`lp_2_s` (screen noise shifts) and `bgrh_scle` (the fill's end point) reach the eight 8-vertex meshes.

Inputs (UI pad indices; mapping from the icons of each row): 0xC/0xD d-pad up/down: selector 4..11 with wrap (4 = off, 5..11 = language 0..6), animation `subttl_sel_<sel-3>`, and after 0.5 s without input the UI language switches to the selected language (not for off); 3 (triangle): toggle option +9 (row A, Vertical), `camera_a_sel_1/2`; 0 (square): toggle option +0xA (row B, Horizontal), `camera_b_sel_1/2`; 0xE/0xF d-pad left/right: brightness 0..10, `bgrh_scle` time = value / 10; 0xB (R3, zoom) while `IsPhotoOptionPending`: press plays `zoom_in`, release sets a flag, and when `zoom_in` has ended with the flag set: `zoom_out` and nazo condition `PhotoOption`. Close: the OPTIONS button toggles the menu (0x9203E0), plays `setout`, then commits subtitles on/off and the language and saves the options.

Opening (0x12856B0): selector = subtitles off ? 4 : language + 5; camera and brightness from the options; `setin`, the two loops and the current selection animations.

Note on the options object: +9 is the vertical and +0xA the horizontal inversion (row labels above); the save block keeps them at +0x2C and +0x2D in that order.

## PC settings page (port addition)

The option screen gets one more row and a second page drawn from its own UIF, so the PC settings look like part of the menu. Every node below is a copy of an existing one with a new name, so the selection animations do not reach it, and the material name kept, so the `lp_1_s`/`lp_2_s` noise shifts do.

- Entry: a copy of the Horizontal row (94) at (32, -26), 8 units under it, with copies of its square icon and glow (104, 106) and its label (108) reading "PC Settings". Its icon is a cross: the game's atlas (`cmn_btn_icon_a_ps4_alp`, BC3) has no cross or circle, so `UiAssets::BuildPcIcons` decodes the atlas and its glow (`cmn_btn_icon_a_ps4_blr`, BC1), takes the square cell (white disc of radius 26 around the cell centre, soft shadow, symbol black at alpha 226 with 4 px strokes), clears the symbol and draws a cross and a circle in the same stroke; the glow is the square's. Cross opens the page (it does nothing on the original screen), as does a click on the row; F10 and the pad's View/Share/Create button open the pause menu right on the page where the game can pause.
- Page: a copy of the content group (39) under node 1, so `setin`/`setout` open and close it with the menu and the dark background (2, 37, 155, 158) stays. It holds a title (copy of 150, "PC SETTINGS"), section headers (41) with their lines (74) at the header positions of the two columns (x -50 and 15, lines 16 units right and 1 below), rows 3 units apart starting 5 below their header with a left-aligned label (41) and a value (entry group 120 at 0.3 or 1 with text 121, as the subtitle list), the camera row's selection bar (80, 81) under the selected value with the flash of `subttl_sel`'s node 136 (alpha 0, 1, 0.667, 0.5, 1 over 9 frames) on every move, a help line (copy of the tip 48, 110 units wide) with the selected row's note, and Back (113, 115, 116) with the circle.
- Switching pages fades the content group out on `setout`'s alpha curve (9 frames) and the other one in on `setin`'s (13 frames), on the menu's 30 Hz clock; `setin_s` plays again when the option screen returns. Input waits for the switch as it waits for `setin`.
- Input: d-pad, left stick, arrows or WASD move (0.4 s before a held direction repeats, then every 0.1 s), left and right change the value, cross, Enter, Space or E change it or run an action (an action asks once more before it runs), circle, Backspace, Esc or OPTIONS return to the option screen. The mouse pointer shows while the menu is open: hovering selects a row, a click changes it, a right click returns. On the option screen a click on a camera row's label toggles it and on a value sets it, a click on a subtitle entry selects it, on the gauge sets the brightness, on Back closes the menu.
- Sounds: `Play_sys_cursor_01` on every move and change, as the option screen's rows; `Play_sys_change_01` (index 3 of the screen's table) when the page opens and when an action runs.
- Graphics, the last row of the Upscaling section (a section of its own would reach the help line), reads More and opens the graphics page on cross, Enter or a click (a link row: no confirmation); circle, Backspace or a right click there returns to the first page with the cursor on its first row, while OPTIONS and Esc (the pause input) return to the option screen from either page. The page has the same layout: Ray tracing on the left with Ray-traced shadows (Off, Sharp or Soft, `[raytracing] shadows` 0, 1 or 2), Contact shadows and Ambient occlusion (Off or On, `[raytracing] contact_shadows` and `ambient_occlusion`; `rendering.md` 12.21), and Textures on the right with Anisotropic filtering (Off, 2x, 4x, 8x, 16x, `[graphics] anisotropy` 0 to 16, applied at once; `rendering.md` 12.22). Every default is Off, the original's look. The ray tracing rows' help line says what the value does, "Takes effect at the next start." when the game started with every ray tracing option off (the device gets the ray query extensions only at start), and on GPUs without Vulkan ray queries they are greyed with "Needs a GPU with Vulkan ray tracing." The ray traced reflections experiment has no row by decision (pt.ini `[raytracing] reflections` only): it changes the original's look by design, and the traced floors show mirror patches the original fades out (under the f060 sink, the cabinet front). `PcSettingsSource::Opened` puts the source on its first page when the menu opens the page, `Back` takes it from a sub-page back to the first one. `tests/pad/pc_graphics.txt` drives the page with a pad.
- Texts are keys resolved by `PcText` with English for every language so far; values that are not keys (sizes, device names) show as they are.
- Rows (`PcSettings` in `src/main.cpp`), each saved to pt.ini at once. Display: display mode (Window, Borderless, Fullscreen, applied at once as with Alt+Enter), resolution (window sizes from 1280 x 720 up to the desktop, applied at once; fullscreen shows the desktop size, greyed), v-sync (applied at the next frame; Off presents with IMMEDIATE, frames shown as they finish and tearing possible, or MAILBOX where the surface has no IMMEDIATE, and its help line says so; FSR 3 frame generation presents with FIFO and DLSS Frame Generation without v-sync whatever the row), pause and mute in background. Upscaling: upscaler (Off, FSR 3, FSR 4, DLSS, XeSS; one this machine or this build cannot run shows greyed while the cursor rests on it, with its reason on the help line, and the setting keeps its value: `PcSettingRow::value_notes`; FSR 4 is always greyed, `upscaling.md`), quality (Native to Ultra performance, greyed without an upscaler), sharpness 0 to 10 (FSR only), or with DLSS the DLSS model in its place (Auto, K, L, M), frame generation (Off, FSR 3, DLSS: AMD FSR 3 or NVIDIA DLSS Frame Generation; a choice this machine cannot run shows greyed with its reason, FSR 3 without a window or `amd_fidelityfx_vk.dll`, DLSS on a GPU before RTX 40, without hardware-accelerated GPU scheduling or in a build without Streamline (`upscaling.md`); DLSS takes effect at the next start, and while Streamline is loaded FSR 3 waits for a restart; the row is greyed while the upscaler is off; while DLSS Frame Generation runs the v-sync row notes that it presents without v-sync). Sound: volume 0 to 200 %, microphone (system default or a recording device). Voice recognition has no switch: it is always on and listens only on f160, as the original, which has no way to the true end without the word (`gameplay.md` 7); an older pt.ini's `[voice] enabled` is ignored and no longer written. Controls: mouse sensitivity (0.25 to 5), camera tilt (On writes [camera] roll 1, the original's lean, Off 0; applies at once), vibration, stick dead zone (0 to 0.30, 0.10 is the DualShock 4's). Progress: reset progress deletes the save after a second cross and then shows Done; the next start is a first boot.

### Button prompts (port addition)

The option screen's button pictures, its PC Settings entry, the PC settings page's Back and the page's help line follow the device used last (`gameplay.md` 14.7). Every picture keeps its node: `OptionsMenu::ApplyPrompts` sets the node state's base texture, UVs and positions (`UifNodeState::texture`, `uvs`, `positions`) of the picture and of its glow, or clears them.

| Row | Nodes (picture, glow) | PlayStation | Xbox / other pads | Nintendo | Keyboard and mouse |
| --- | --- | --- | --- | --- | --- |
| Brightness | 44, 46 | D-pad left/right (data) | the same | the same | the arrow keys, left and right lit |
| Subtitles | 141, 144 | D-pad up/down (data) | the same | the same | the arrow keys, up and down lit |
| Vertical | 89, 91 | triangle (data) | Y | X | Enter (menu confirm) |
| Horizontal | 104, 106 | square (data) | X | Y | Backspace (menu cancel) |
| Back | 113, 116 | OPTIONS (data) | menu (three lines) | + | Esc |
| PC Settings (entry) | copies of 104, 106 | cross (drawn from the data) | A | B | F10 |
| Back (PC page) | copies of 113, 116 | circle (drawn from the data) | B | A | Esc |
| Help line (PC page) | copy of 48 | cross | A | B | Enter |

- Pads by position, with the letters SDL gives those positions (`SDL_GetGamepadButtonLabelForType`): south is cross on PlayStation pads, A on Xbox and other pads and B on Nintendo pads; east circle, B, A; west square, X, Y; north triangle, Y, X. The keyboard names come from the bindings table the input reads (`KeyBindings`, first binding of the action), in the current keyboard layout (`SDL_GetKeyFromScancode`, so the W key is Z on AZERTY) and shortened as keycaps are (Escape Esc, Return Enter); an action first bound to a mouse button shows a mouse with that button filled, and a D-pad pair that is not two arrow keys shows both names on one keycap.
- The generated pictures are drawn once, when first shown (`UiAssets::PromptPicture` in `src/game/ui/ui_icons.cpp`), in the data's style and from the data: the square cell of `cmn_btn_icon_a_ps4_alp` and of its glow `cmn_btn_icon_a_ps4_blr` give, averaged over rings, the white disc's radius (26.2 pixels of the 128 pixel cell), its edge coverage, the black shadow's alpha around it (0.88 at the edge, 0.43 two pixels out, 0.05 eight out) and the glow's level by distance from the edge (0.99, 0.62 four pixels out, 0.27 eight out), and the symbol's alpha (0.88); every generated body (disc, keycap, arrow keys, mouse) takes that edge, shadow and glow by its own distance field, so it is lit and shaded as the data's discs are. Letters are the game's system font (`font_def_ltn.ffnt`, 53 pixel em, one bit per pixel) through distance fields built from its bits, grown by 0.9 pixels towards the symbols' 4 pixel strokes (the keycaps' names by 0.5), 22 pixels high in the disc (as its triangle and square); the menu lines and the plus are 4 pixel strokes as the atlas's symbols. Keycaps are rounded rectangles as high as the disc (radius 9) and as wide as their name plus 12 pixels a side; the arrow keys are four 21 pixel keys, the lit ones white with a black arrow and the others grey at alpha 0.74 as the D-pad glyph's unused arms (rgb 75, alpha 188).
- Cells are drawn at twice the atlas's resolution (256 pixels high, with mipmaps), so they stay sharp where a 5 unit icon is 150 pixels high (2160p). A cell is as wide as its body plus the disc cell's margins; the picture node's quad is widened to the left by the cell's width over its height and its UVs cover the cell, so the picture's right edge stays where the data's disc ends and a keycap grows away from its label.
- The help line: `pc_note_reset_confirm` reads `Press {accept} again to delete the save.`; the token becomes U+E000, which the text layout lays out as a gap of the picture's width (`TextStyle::inline_advance`), and `UifView` draws the picture there with its glow (additive at 0.4 as the icon glows), its disc or keycap one em high and centred on the line's capital height.

### Buttons in the game's textures (port addition)

One world texture shows a button: the R3 chalked on the photo in the fallen frame (FrameGround on f005, the zoom: `shsb_labl001_p1_bsm_alp`, 1024 pixels, BC3, the strokes in alpha over one colour, mapped by `shsb_labl001_rthr001` over u 0.050..0.539, v 0.250..0.582). `PromptTextures` (`src/game/prompt_textures.cpp`) makes it follow the prompt device as the menus do:

| Texture | PlayStation | Xbox / other pads | Nintendo | Keyboard and mouse |
| --- | --- | --- | --- | --- |
| fallen frame (zoom, R3) | R3 (data) | RS | R and a stick | a mouse with the right button lit (the zoom's first binding) |

- The blue X painted over Lisa's photo (the XMark photo on f050, `shsb_hous001_p3_bsm` and its specular map `shsb_hous001_p2_srm`) is not a button prompt: it is the story's mark, the husband crossing Lisa out, and it stays the data's on every device (an earlier port version painted A, B or a mouse over it). The gouge itself is X on every device (gameplay.md, keyboard and pads).
- The chalk variants rub out the 3 (both characters for the mouse) in alpha and chalk new strokes in the R3's style: 25 pixels wide, starting in a few bristles, solid after. A binding a picture cannot show keeps the data's button.
- Every level is patched: level L gets level 0's change averaged over its 2^L squares added to its own decoded pixels, and only the 4 x 4 blocks that change are encoded again (BC3's alpha per block), so everything else keeps its data bit for bit.
- The copy is a new texture and the materials that use the original are pointed at it (`TextureManager::RedirectTexture`, applied when materials are flushed); PlayStation pads point them back. With a window a painting runs on a worker thread (about 30 ms) and the copy shows when it is ready; headless runs paint at once, so tests see the switch in the frame of the device change (`PT_PROMPT_TEXTURE_WORKER=1` takes the worker there too). `PT_PROMPT_TEXTURE_DUMP=<folder>` writes the texture's level 0 once and the painted box of each variant as PNG.

## Port differences

- The option menu's animations advance once per 30 Hz game frame, 2 UIA frames per step: the `lp_1_s` and `lp_2_s` loops (10 UIA frames) then show only their even keys, as the menu_detail_rb watch log does (the bar screen shifts cycle (0.323, -0.376), (-0.290, 0.264), (0.419, -0.424), (-0.242, 0.344), (-0.484, 0.312) with a period of 5 frames: keys 2, 4, 6, 8, 0). With the port's 60 Hz steps the odd keys (3, 5, 7) showed as extra noise positions. Against the capture, shifted by the 3 frames of input latency below, the setin title (51, 87, 123, 156, 190, 230, 254 against 53, 89, 123, 157, 193, 232, 255) and the bar noise (36.2, 41.1, 33.5, 47.7, 60.5 against 35.3, 40.1, 32.7, 46.7, 59.5) and setout (192, 144, 94, 41 against 194, 143, 95, 42) now match frame for frame.
- Input latency: the captures show the menu's reactions 3 frames after the frame a button is pressed in (menu_detail_rb: the first setin frame is 1104 for OPTIONS at 1100, the gauge moves at 1193, 1223, 1253 and 1343 for presses at 1190, 1220, 1250 and 1340, the first setout frame is 1374 for OPTIONS at 1370; the first-boot screen's first setout frame is 2430 for OPTIONS at 2426 in preface_detail_rb), the same 3 frames that walks and camera turns show (gameplay.md 10.3). It is the capture's latency for any input (one frame of it is the numbering: a pad value of frame N is read by the logic of frame N + 1), not a delay of the menu; the port reacts in the tick of the press and matches the capture frame for frame 3 frames earlier (the setin, gauge and setout frames above, the darkening of the scene behind the menu and the world pause from the first setin to the first setout frame).
- The option menu plays its UIA files like the original (`setin` with `setin_s`, the `lp_1`/`lp_2` loops, the selection, zoom and `setout` animations, `bgrh_scle` held at brightness / 10 with its vertex track); against the shadPS4 captures every text and picture lands on the same pixels (bar extents within 2 px, gauge rows 624 to 635, fill edge at x 550), and the gauge and header lines match the first menu frame of explore_boot in shape. The captures made with the texture fix (menu_detail_rb, preface_detail_rb) confirm it: a solid gauge (fill 255, background mean 34.6 against the port's 34.4), the header lines (p99 38 against 41) and the noisy bars (core mean 74.0 against 74.3). The film grain drawn after the menu (rendering.md, film grain) is what lifted the dim option texts (p99 59 against the port's former 52), the lines and the brightness images (mean 7.3 against the former 5.8) in the captures; with it the port matches them. The texts match too: against the menu target of menu_trace_rb frame 1180 before the grain (dumped after the last text draw) the title, the headers, the tip line and the option texts differ by RMSE 0.00 to 0.08 with the same ink, where the port's former 4-level trilinear atlas without the half texel gave 2 to 7 % more ink and RMSE 17 to 22 on the white texts. `get_chip` is not played (no caller found).
- Demo UI graphs: the graph node class names are not recovered and only the six classes of the P.T. graphs run; all layouts of a graph draw at 181 in graph order.
- Subtitle glyph size is the `sbt-sys-M` style size (22 x 22, likely, see above). Their `Draw2D_Border` rim (`ShadeBorder` in `shaders/ui_sprite.frag`) keeps its taps at 1.2 pixels of the original's 1080p (the derivatives are scaled by the output height over 1080), so at other output sizes the rim keeps its share of the glyph; at 1080p it is the original's.
- Subtitle distance: the radius rule is implemented (the listener is the game camera), but subtitle requests carry no emitter position, so an entry with range 1 to 3 would stay hidden, and the camera cone scaling is not applied; no P.T. entry uses a nonzero radius.
- Button prompts follow the device used last (button prompts above): the PS4 icons of the data for PlayStation pads, generated ones for the others. Keyboard: arrows = d-pad, Enter/Space/E = triangle row, Backspace = square row, right mouse button held = zoom (R3), Escape = OPTIONS.
- Brightness: the original sets a video output gamma (see Option menu); the port's final composite applies `rgb^(1 / value)` to the whole frame, UI included, so port screenshots at level 5 are darker in the mid tones than the captures, which are taken before that gamma. Compare with `PT_OUTPUT_BRIGHTNESS=1`. The brightness images get their level as an RGB scale where the original sets their color alpha; with their additive blending the result is the same.
- The subliminal noise samples the full-resolution scene instead of the 512 x 512 capture.
