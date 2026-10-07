# Demo streams (.fsm)

P.T. cutscenes ("demos") are Fox demo streams: a chunk sequence with interleaved motion, event and audio packets. This
page gives the layout of the `DEMO` packets; the same key codec and node tree are used by the motion archives
(`motion.md`). Tool: `tools/fsm.py` (listing, `--extract DIR` for the Wwise stream, `--json DIR` for the full decode
into `<dir>/<name>.json` and `summary.txt`). Give it `--fox2` (the `fox2.py` JSON root, for the DemoData) and
`--models` (the `fmdl.py` JSON root, for bone names) to name actors and bones.

Marks: confirmed = verified on the data of all 41 files or read from the game's code; likely = consistent with all data;
guess = unverified.

## Files

| file | where | notes |
| --- | --- | --- |
| 40 `gc_p*.fsm` | resident package (`resident.fpk`, `/Assets/sh/demo/demo_stream/`) | DemoData `onMemory` true |
| `gc_p06_010_final.fsm` | loose in `chunk1.psarc` | the ending; DemoData `onMemory` false (`gc_p06_010.fox2` in `ending.fpkd`) |
| `#Eng/gc_p06_010_final.fsm` | loose, language folder | the same demo data plus `SYS ` and 30 `SND ` chunks (English voice) |

## Chunks

| tag | layout | notes |
| --- | --- | --- |
| `DEMO` | char[4], u32 size (incl. 16-byte header), f64 time (s), payload | payload starts with u32 packet type: 1 header, 0 motion block, 2 event block |
| `SND ` | char[4], u32 size, f64 time, Wwise data | the first one has 16 extra bytes (u32 total size, u32 2, 8 zero bytes); `audio.md` |
| `SYS ` | char[4], u32 size (16), 8 bytes | first chunk of streams with audio |
| `END ` | char[4], u32 16, f64 time | stream end |

Order: one type 1 packet at time 0, then per time step a type 0 packet, optionally followed by a type 2 packet with the
same time, with `SND ` chunks interleaved by time.

Time base: demo frames at 60000/1001 = 59.94 fps. Chunk times are `startFrame * 1001/60000`. Key deltas are counted in
frames; the runtime time unit is 1/5 frame ("ticks per frame" 5 in every units table).

## Loops

Streams whose motion is split into 30-frame blocks hold the whole timeline three times back to back (frames 0..L,
L..2L, 2L..3L, L = DemoData `demoLength`); each copy ends with a `DemoEnd` event, the second and third copies repeat the
motion blocks byte for byte (except the first and last block of a copy) and omit `DemoStart` and the `Create*` events.
The game ends a demo at the first `DemoEnd` unless loop mode is set. Streams with 300-frame blocks (the timeline-only
demos gc_p02_060/070/080/090/100/500/520, gc_p03_070, gc_p04_280/290) hold one copy. `END ` time = end of the last copy
(plus audio tail when `SND ` is present).

## Packet type 1: header (node tree)

Payload: u32 1, three u32 0, then the root node at payload + 0x10.

### Node (0x30 bytes, then name)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | name hash: StrCode32 (low 32 bits of StrCode64) of the name |
| 0x04 | u32 | name offset from the node (0x30), 0 = unnamed (gani nodes) |
| 0x08 | u32 | data type: 1 units table, 3 event table (gani), 0 none or name table |
| 0x0C | i32 | data offset from the node (0x40 named, 0x30 unnamed), 0 = none |
| 0x10 | u32 | data size (gani and name tables; 0 for units in stream headers) |
| 0x14 | i32 | parent node, relative |
| 0x18 | i32 | first child, relative (0 = none) |
| 0x1C | i32 | previous sibling, relative |
| 0x20 | i32 | next sibling, relative (0 = last) |
| 0x24 | u32 | size of everything after the name (units + parameters) |
| 0x28 | u32[2] | 0 |
| 0x30 | char[16] | name, zero padded (named nodes only) |

Parameters follow the units table, 16-byte aligned, or directly follow the name.

### Parameters

Linked records; string offsets are relative to the field holding the corresponding hash.

| offset | type | field |
| --- | --- | --- |
| 0x00 | u16 | value type: 1 string, 2 float |
| 0x02 | u16 | offset to the next record, 0 = last |
| 0x04 | u32 | key: StrCode32 of the key name |
| 0x08 | u32 | key string offset (from +0x04), 0 = no string |
| 0x0C | u32 | string: StrCode32 of the value; float: the value |
| 0x10 | u32 | string: value string offset (from +0x0C) |

Keys in P.T.: `TARGET_NAME` (0x9932327B), `SLOPE_DIR` (0xCC39A1F6) and `SLOPE_ANGLE` (0x021922A7) on `MOTION` (always 0.0).

### Units table (animation channels)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | unit count |
| 0x04 | u32 | track count (in stream headers: tracks of this node; the global track indices are in the entries) |
| 0x08 | u32 | 0x01000000 (0x01000001 in rig-driven gani, `motion.md`) |
| 0x0C | u32 | frame count (stream headers: the first block's count) |
| 0x10 | u32 | ticks per frame, 5 |
| 0x14 | u32[unit count] | unit offsets from the table |

Unit:

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | unit hash: StrCode32 of a bone name (`SKL_000_WAIST`...) or a fixed name |
| 0x04 | u8 | track count |
| 0x05 | u8 | flags: bit 0 loop at the end (else hold the last key), bit 1 cubic vector interpolation, bit 2 all tracks static (gani) |
| 0x06 | u16 | 0 |
| 0x08 | 8 bytes x count | track entries |

Track entry:

| offset | type | field |
| --- | --- | --- |
| 0x00 | i32 | data offset from this entry (gani inline data); 0 in stream headers |
| 0x04 | u16 | track index: slot in the motion block offset table |
| 0x06 | u8 | bits 0-3 kind, bit 7 another track follows in this unit |
| 0x07 | u8 | bits per component: 18 rotations, 32 floats in all demos; 12, 13, 15 in gani |

Track kinds: 0 rotation (quaternion keys); 1, 2, 3, 4 vector of 1 to 4 components; 5 rotation with slerp setup; 6
3-component vector (gani root translation). Demos use only kind 0 with 18 bits, kind 3 with 32 bits, and a few kind 1
and 2 tracks.

### Node vocabulary (demo headers)

| node | parent | contents | meaning |
| --- | --- | --- | --- |
| `ROOT` | | | root |
| `DEMO` | ROOT | | timeline |
| `CAMERA` | DEMO | `MOVE` + `CameraParam` | the demo camera |
| `MOVE` | CAMERA | unit `Transform` (0x961C702E): rotation, translation; `TARGET_NAME demo_camera` | camera transform |
| `CameraParam` | CAMERA | unit 0xAAA49369: 1 float; `TARGET_NAME CameraParam` | focal length in mm (below) |
| `SI Frame` | DEMO | unit 0x7064158A: 2 floats | Softimage source frame and a rate (1.0); editor reference (likely) |
| `LOCATOR` | DEMO | `MOVE` per locator, `MESH_EVENT` | animated locators: static models (`ENV_*`) and effect nulls (`Eff_Null_*`) |
| `MOVE` | LOCATOR | unit `Transform`: rotation, translation; `TARGET_NAME` = locator name | locator transform |
| `MESH_EVENT` | DEMO or LOCATOR | empty | placeholder; mesh visibility comes as `VisibleMesh` events |
| `MOTION` | DEMO | `SLOPE_DIR`, `SLOPE_ANGLE`; per skinned model `SKEL`, `MOVE`, `MODEL`, `MTEV` (and `MTP` in the ending) | skinned models |
| `SKEL` | MOTION | one unit per bone (StrCode32 of the fmdl bone name): rotation and, where animated, translation | bone local transforms |
| `MOVE` | MOTION | unit 0x01AD535D: rotation, translation; `TARGET_NAME` = model | model root transform (same unit hash as the gani root) |
| `MTP` | MOTION | units `MTP_RHAND_A`, `MTP_RHAND_B`, `MTP_LHAND_A` | motion points (attach transforms), ending only |
| `MODEL`, `SKELINFO`, `MTPINFO`, `MTEV` | MOTION | empty | placeholders |
| `MOTION` without `TARGET_NAME` | DEMO | unit 0xC0776230: 1 float, constant 0 | dummy timeline of the event-only demos |
| `SHADER` | ROOT | `TARGET_NAME` only, no units in any file | shader parameter tracks: none in P.T. |
| `MTP_LIST`, `MTP_PARENT_LIST` | ROOT | name tables (u32 count, then u32 hash + u32 string offset from the entry) | motion point names and parent bones, ending only |

Actors are named by `TARGET_NAME` and bound through the DemoData in the fox2 files: `demo_camera`; `HM_plr0_main0_def`
(the player); `HM_och0_main0_def` (Lisa); `HM_coc0_main0_defN` (cockroaches); `HM_shl0_main0_def` (flashlight); `ENV_*`
static models; `Eff_Null_*` effect nulls. Bone units resolve against the fmdl bone names.

## Packet type 0: motion block

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | 0 |
| 0x04 | u32 | flags: bit 0 an event packet (type 2) with the same time follows (likely), bit 1 per-track flag table present, bit 2 offset vector present |
| 0x08 | u32 | start frame |
| 0x0C | u32 | frame count (31 for the first block of a copy, then 30; 300 in timeline demos; last block shorter) |
| 0x10 | u32 | track count n (all tracks of the header) |
| 0x14 | u32 | payload size (chunk size - 16) |
| 0x18 | u32 | 0, or the byte size of this packet plus the event packet of the same time (likely a prefetch size) |
| 0x1C | u32[n] | track data offset from the payload start, 0 = no data |
| | f32[3] | offset vector (flag bit 2); (0, 1, 0) in most streams, whole-metre vectors that follow the shots in the endings |
| | u16[n] | track flags (flag bit 1) |
| | | track data: byte-aligned key streams |

Track flags:

| bit | meaning |
| --- | --- |
| 0 | static: one key |
| 1 | on the first track of a unit list (one model): the model has no data in this block; all its tracks are skipped |
| 2 | add the offset vector to the decoded values. A packet without a vector keeps the last one an earlier packet stored; only the endings mix the two |
| 3 | not evaluated in this block: the channel keeps its previous state |

A block spans frames `start` to `start + count` inclusive: its last key sits on the next block's first frame and equals
that block's first key.

## Key streams

A track's data is a little-endian bit stream. Layout: key 0, then pairs (u8 delta frames, key) until the deltas add up
to the block's frame count; static tracks hold key 0 only. Keys sit at frames 0, d0, d0+d1, ...; values between keys
are interpolated.

Rotation key (kinds 0 and 5), `3*b + 3` bits:

| bits | field |
| --- | --- |
| b | angle a |
| b | axis x |
| b | axis y |
| 3 | sign bits: bit 0 negates x, bit 1 y, bit 2 z |

```
s = 1 / (2^b - 1)
x = s * X, y = s * Y, z = 1 - x - y            axis on the positive octant, |x| + |y| + |z| = 1
len = sqrt(x*x + y*y + z*z)                    (1 if len*len <= 1.1754944e-38)
h = s * A * pi / 2                             half angle in [0, pi/2]
q = (sx * x * sin(h) / len, sy * y * sin(h) / len, sz * z * sin(h) / len, cos(h))   w >= 0
```

With b = 18 a key is 57 bits; a static rotation takes 8 bytes. The fields are unsigned.

Vector key (kinds 1-4, 6): `components * b` bits, each component an IEEE float (b = 32) or a 16-bit float with exponent
bias 8 instead of the IEEE half's 15 (b = 16: `sign << 31 | ((exponent << 13) + 0x3B800000`; exponent 0 decodes to 0, no
infinities). With the IEEE bias the player's start clips come out 128 times too small.

Interpolation: vectors linear; rotations spherical with the shortest arc. End of a track: units with loop flag 0 hold
the last key, loop flag 1 wraps to key 0 and accumulates a root delta.

## Skeleton pose

Bone channels are local to the parent bone. The fmdl skeleton has no bind rotations (`fmdl.md`), so a rotation channel
is the bone's local rotation. Translation channels are offsets added to the bind local translation (fmdl world minus
parent world): every non-root translation channel of the player and Lisa skeletons is exactly 0, and the root bone
(`SKL_000_WAIST`, bind position 0) carries the hip height. Bones without a channel keep the bind pose. The model root
channel (`MOTION/MOVE`) places the whole model in demo space: `model = demo transform x root x bone chain`.

## Camera

| channel | source | notes |
| --- | --- | --- |
| position, rotation | `CAMERA/MOVE` | demo space; forward is the camera's local -Z, up local +Y |
| focal length | `CameraParam` float (13, 18.8, 20, 35, 48 mm in the data) | film gate 24 x 13.5 mm: vertical FOV = 2 atan(6.75 / f), horizontal = 2 atan(12 / f) |
| near clip | 0.05 m | set when the demo camera is created |
| far clip, exposure, bloom, shutter, depth of field | copied from the game camera at start and driven by `ExecCommand` events; no track in the streams |

The first camera key equals DemoData `cameraStartTranslation`/`cameraStartRotation` within a few mm and the last key
equals `cameraTranslation`/`cameraRotation` exactly.

## Packet type 2: events

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | 2 |
| 0x04 | u32 | payload size |
| 0x08 | u32 | 0 |
| 0x0C | u32 | event track hash 0xC8F4FCB6 (the default track) |
| 0x10 | u32 | event count (read as u16) |
| 0x14 | u32[count] | record offsets from +0x0C |

Record:

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | event type (StrCode32) |
| 0x04 | u8 | bits 0-5 section count, bits 6-7 section size: 0 two i32, 1 two i16, 2 two i8 |
| 0x05 | u8 | int count |
| 0x06 | u8 | float count |
| 0x07 | u8 | string count |
| 0x08 | | sections (start frame, end frame); i32 form: -1 = no end (instant event), bit 30 = boundary outside this block |
| align 4 | u32[ints] | ints (at most 48) |
| | f32[floats] | floats (at most 32) |
| | u64[strings] | StrCode64 values in the low 48 bits (at most 16) |

Event types (StrCode32 of the names):

| type | hash | payload | notes |
| --- | --- | --- | --- |
| `DemoStart` | 0xD3185DFF | int (0 and 1, two events at frame 0) | |
| `CreateCamera` / `DeleteCamera` | 0x7F4D8E71 / 0xF4603510 | int 2 | frame 0 / L |
| `CreateModel` / `DeleteModel` | 0xC5806267 / 0x75586EE7 | ints (1, 0) static or (0, 1) skinned model, strings (model name, fmdl path, then 4 empty for skinned) / string (model name) | |
| `CreateLocator` / `DeleteLocator` | 0x966D8EC3 / 0x3148F136 | string (locator name) | |
| `VisibleModel` | 0x3E09E7B9 | int visible, string model | |
| `VisibleMesh` | 0x6B491E18 | int visible, strings model, mesh | |
| `ClipEnd` | 0x2A36C514 | int | at L - 1 |
| `DemoEnd` | 0xD2A6999A | none | at L |
| `ExecCommand` | 0x2379C011 | functor call (below) | 7,637 records in the 41 streams (all copies) |

### ExecCommand

The source form is visible in the two event files (`.evf`, fox2 `EventDataUnit` entities with `eventName ExecCommand`,
`paramString`, `paramInt`, `paramFloat` and a `TimeSection`): the compiled record stores `paramInt`, `paramFloat` and
StrCode64 of `paramString` in the same order.

Strings: the parameter values, then 5 fixed strings: clip reference (`konShotCameraN;start;end;offset`), empty,
`DemoEvent_<functor>`, `<functor>`, category (`<group>;<n>;<name>;`). The handler is looked up with the second-to-last
string.

Ints: raw int values, then one descriptor pair per parameter, then a footer (layout likely: the grammar below parses all
7,637 records):

| word | meaning |
| --- | --- |
| raw ints | values of int, bool and enum parameters |
| (u32 key, u32 type << 16 \| index) per parameter | key = StrCode32 of the parameter name; index into the value array of the type |
| plain footer: (3 \| n << 16), (2 \| 1 << 16) | n plain parameters |
| interpolated footer: (c \| k << 16 \| 1 << 24), (4 \| m << 16), (2 \| 2 << 16) | the first k descriptors are interpolated between the section start and end and store start and end values back to back; m plain ones follow; c unknown |
| length footer: (5 \| n << 16), -1, frame, (4 \| 1 << 16) | the 4 skip events (flag 2): n parameters and the frame they wait for |
| last: (6 \| flags << 16) | flags 2: skip event; 8: interpolated start values from the functor's start getter; 0x10: end values from its end getter |

Parameter types (high 16 bits): 0x0B string, 0x16 string (path), 0x08 float, 0x0A bool, 0x05 int (Wwise ids), 0x04 int,
0x13 color (4 floats: RGBA), 0x0F vector4, 0x10 vector4, 0x0E, 0x020E, 0x040E vector3, 0x0210 quaternion, 0x14 file
(index into the strings).

90 functors are used by P.T. streams. Demo messages are `ExecCommand` events with functor `DemoSendMessageFunctor` (0xCEE2EDD67199) and a string
parameter `message` (StrCode32 0x73F3ECE4); the game's Lua message scripts receive them.

## Messages in the streams

| demo | frame | message |
| --- | --- | --- |
| gc_p00_020 | 522 | hideOcho |
| gc_p00_022 | 400, 470 | OpenDoor20, FinishMotion |
| gc_p01_010 | 1, 280 | InvisibleStaticModel, FinishMotion |
| gc_p01_011, 020, 021, 050, 070, 081, 090, 100, 110 | L - 3 | FinishMotion |
| gc_p01_022 | 149, 295 | PadEnable, FinishMotion |
| gc_p02_080 | 5396, 5875 | DisableOption, Endf120 |
| gc_p02_520 | 0, 118 | PlayRadio, PlayGimmick |
| gc_p04_120 | 130, 1406 | enable_vfx_dust_glass, FinishMotion |
| gc_p04_280 | 531 | enable_voice_breath |
| gc_p04_290 | 365 | StrCode64 0xAA36200CF879 (unresolved; no script listens to it) |
| gc_p05_010 | 3594 | FinishMotion |
| gc_p07_030 | 1306 | GotoGameOver |

Later copies repeat the messages at +L and +2L.

## Output of tools/fsm.py --json

`<dir>/<name>.json`: chunk counts, `length`, `loops`, `demoData`, `tree` (nodes with params), `actors`, `tracks`
(decoded keys of the first copy), `blocks`, `camera.frames` (frame, position, quaternion, focal length), `transforms`
(per frame for locators, model roots), `skinned` (per frame, per bone quaternion and translation offset as stored;
null where the bone has no channel) and `events` (first copy; ExecCommand decoded into functor, params, interpolation,
length). `summary.txt` lists every demo.
