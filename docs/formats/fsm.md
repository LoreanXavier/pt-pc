# Demo streams (.fsm)

P.T. cutscenes ("demos") are Fox demo streams: a chunk sequence with interleaved motion, event and audio packets. This page gives the complete layout of the `DEMO` packets. Runtime behaviour (DemoDaemon, playback, messages, camera and player handoff) is in `../demo.md`; the shared key codec and node tree are also used by motion archives (`motion.md`). Tool: `tools/fsm.py` (listing, `--extract` for the Wwise stream, `--json DIR` for the full decode into `dump/demo/<name>.json`, `summary.txt` and the placement check `validation.txt`).

Marks: **confirmed** = read from code (address given) or verified on the data of all 41 files; **likely** = consistent with all data, code not traced; **guess** = unverified.

## Files

| file | where | notes |
| --- | --- | --- |
| 40 `gc_p*.fsm` | resident package (`resident.fpk`, `/Assets/sh/demo/demo_stream/`) | DemoData `onMemory` true (resident_demo.fox2) |
| `gc_p06_010_final.fsm` | loose in `chunk1.psarc` (`as/sh/demo/demo_stream/`) | ending; DemoData `onMemory` false (gc_p06_010.fox2 in `ending.fpkd`) |
| `#Eng/gc_p06_010_final.fsm` | loose, language folder | the same demo data plus `SYS ` and 30 `SND ` chunks (English voice) |

`gc_p07_020` is referenced by level data but has no DemoData and no stream (confirmed, `levels.md`).

## Chunks

| tag | layout | notes |
| --- | --- | --- |
| `DEMO` | char[4], u32 size (incl. 16-byte header), f64 time (s), payload | payload starts with u32 packet type: 1 header, 0 motion block, 2 event block (confirmed) |
| `SND ` | char[4], u32 size, f64 time, Wwise data | first one has 16 extra bytes (u32 total size, u32 2, 8 zero bytes); `audio.md` (confirmed) |
| `SYS ` | char[4], u32 size (16), 8 bytes | first chunk of streams with audio (confirmed) |
| `END ` | char[4], u32 16, f64 time | stream end; the stream reader maps the tags `END ` and `SYS ` to 0 (no packet plugin) (0x1094BD0, 0x1094F30, confirmed) |

Order: one type 1 packet at time 0, then per time step a type 0 packet, optionally followed by a type 2 packet with the same time, with `SND ` chunks interleaved by time (confirmed). The stream reader (0x1085000-0x1098000) dispatches packets by tag to plugins (`DEMO` plugin looked up with 0x1089AB0(stream, 'DEMO'), confirmed).

Time base: demo frames at 60000/1001 = 59.94 fps (0xB13610 and 0xB13B80 multiply seconds by 59.940059940059946; 0xB13610 converts frames with 0.01668333, confirmed). Chunk times are `startFrame * 1001/60000` (confirmed on all files). Key deltas are counted in frames; the runtime time unit is 1/5 frame ("ticks per frame" 5 in every units table; 0xA93970 compares `time` with `startFrame * 5`, 0xAD5D70 multiplies deltas by it, confirmed).

## Loops

Streams whose motion is split into 30-frame blocks hold the whole timeline three times back to back (frames 0..L, L..2L, 2L..3L, L = DemoData `demoLength`); each copy ends with a `DemoEnd` event, the second and third copies repeat the motion blocks byte for byte (except the first and last block of a copy) and omit `DemoStart` and the `Create*` events (confirmed on the data). The game ends a demo at the first `DemoEnd` unless loop mode is set (0xB12F70: at the end with loop flag 4 the body restarts, otherwise it finishes, confirmed). Streams with 300-frame blocks (the timeline-only demos gc_p02_060/070/080/090/100/500/520, gc_p03_070, gc_p04_280/290) hold one copy. `END ` time = end of the last copy (plus audio tail when `SND ` is present).

| demo | L frames | copies | | demo | L frames | copies |
| --- | --- | --- | --- | --- | --- | --- |
| gc_p00_010 | 128 | 3 | | gc_p02_520 | 120 | 1 |
| gc_p00_020 | 868 | 3 | | gc_p03_010 | 1998 | 3 |
| gc_p00_022 | 478 | 3 | | gc_p03_011 | 2998 | 3 |
| gc_p00_030 | 481 | 3 | | gc_p03_012 | 4998 | 3 |
| gc_p00_160 | 228 | 3 | | gc_p03_070 | 60 | 1 |
| gc_p01_010 | 293 (DemoData 283) | 3 | | gc_p04_120 | 1408 | 3 |
| gc_p01_011 | 198 | 3 | | gc_p04_200 | 1998 | 3 |
| gc_p01_020 | 298 | 3 | | gc_p04_280 | 533 | 1 |
| gc_p01_021 | 178 | 3 | | gc_p04_290 | 848 | 1 |
| gc_p01_022 | 298 | 3 | | gc_p04_300, 320, 330 | 6998 | 3 |
| gc_p01_050 | 88 | 3 | | gc_p04_310 | 5198 | 3 |
| gc_p01_070 | 598 | 3 | | gc_p04_340 | 6198 | 3 |
| gc_p01_081 | 58 | 3 | | gc_p04_350 | 332 | 3 |
| gc_p01_090 | 2238 | 3 | | gc_p05_010 | 3597 | 3 |
| gc_p01_100, 110 | 198, 298 | 3 | | gc_p07_030 | 1308 | 3 |
| gc_p02_060, 070 | 30, 60 | 1 | | gc_p06_010_final | 8902 | 3 |
| gc_p02_080, 090, 100, 500, 510 | 5878, 1000, 2280, 940, 1198 | 1, 1, 1, 1, 3 | | | | |

## Packet type 1: header (node tree)

Payload: u32 1, three u32 0, then the root node at payload + 0x10 (confirmed on all files). The same node format is used by gani animations (`motion.md`).

### Node (0x30 bytes, then name)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | name hash: StrCode32 (low 32 bits of StrCode64) of the name |
| 0x04 | u32 | name offset from the node (0x30), 0 = unnamed (gani nodes) |
| 0x08 | u32 | data type: 1 units table, 3 event table (gani), 0 none or name table (0xAB2270, 0xAA7ED0, confirmed) |
| 0x0C | i32 | data offset from the node (0x40 named, 0x30 unnamed), 0 = none |
| 0x10 | u32 | data size (gani and name tables; 0 for units in stream headers) |
| 0x14 | i32 | parent node, relative |
| 0x18 | i32 | first child, relative (0 = none) |
| 0x1C | i32 | previous sibling, relative |
| 0x20 | i32 | next sibling, relative (0 = last) |
| 0x24 | u32 | size of everything after the name (units + parameters) |
| 0x28 | u32[2] | 0 |
| 0x30 | char[16] | name, zero padded (named nodes only) |

Children are found by hash with 0x549430 (FoxDataNode find child, confirmed). Parameters follow the units table, 16-byte aligned, or directly follow the name.

### Parameters

Linked records; string offsets are relative to the field holding the corresponding hash (confirmed).

| offset | type | field |
| --- | --- | --- |
| 0x00 | u16 | value type: 1 string, 2 float |
| 0x02 | u16 | offset to the next record, 0 = last |
| 0x04 | u32 | key: StrCode32 of the key name |
| 0x08 | u32 | key string offset (from +0x04), 0 = no string |
| 0x0C | u32 | string: StrCode32 of the value; float: the value |
| 0x10 | u32 | string: value string offset (from +0x0C) |

Keys in P.T.: `TARGET_NAME` (0x9932327B; StrCode64 0x23AB9932327B is what the runtime matches, 0xA90FA0 and 0x79A740, confirmed), `SLOPE_DIR` (0xCC39A1F6) and `SLOPE_ANGLE` (0x021922A7) on `MOTION` (read by 0xAB0540, confirmed; always 0.0).

### Units table (animation channels)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | unit count |
| 0x04 | u32 | track count (in stream headers: tracks of this node; the global track indices are in the entries) |
| 0x08 | u32 | 0x01000000 (0x01000001 in rig-driven gani, `motion.md`) |
| 0x0C | u32 | frame count (stream headers: the first block's count) |
| 0x10 | u32 | ticks per frame, 5 |
| 0x14 | u32[unit count] | unit offsets from the table |

Unit (0xAAC4A0, 0xAAC980, 0xAAB550, confirmed):

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | unit hash: StrCode32 of a bone name (`SKL_000_WAIST`...) or a fixed name |
| 0x04 | u8 | track count |
| 0x05 | u8 | flags: bit 0 loop at the end (else hold the last key; 0xAD5E70 via 0xAAB550), bit 1 cubic vector interpolation (0x80-byte channel, 0xAD5010/0xAD5510), bit 2 all tracks static (gani, 0xAAC010) |
| 0x06 | u16 | 0 |
| 0x08 | 8 bytes x count | track entries |

Track entry:

| offset | type | field |
| --- | --- | --- |
| 0x00 | i32 | data offset from this entry (gani inline data); 0 in stream headers |
| 0x04 | u16 | track index: slot in the motion block offset table |
| 0x06 | u8 | bits 0-3 kind, bit 7 another track follows in this unit |
| 0x07 | u8 | bits per component: 18 rotations, 32 floats in all demos; 12, 13, 15 in gani |

Track kinds (0xAAB550; component counts from the table at 0x13D3FA0 = 1, 2, 3, 4, 0, 3; channel buffer sizes 0x40/0x60/0x80 from 0xAAC4A0, confirmed):

| kind | meaning | decoder (init static / init / advance) |
| --- | --- | --- |
| 0 | rotation, quaternion keys | 0xAD5D00 / 0xAD5D70 / 0xAD5E70 |
| 1, 2, 3, 4 | vector of 1 to 4 components | 0xAD4530 / 0xAD46F0 / 0xAD4A90 (cubic: 0xAD5010 / 0xAD5510) |
| 5 | rotation with slerp setup (precomputed arccos) | 0xAD6220 / 0xAD62B0 / 0xAD6640 |
| 6 | 3-component vector (gani root translation) | as 1-4 |

Demos use only kind 0 with 18 bits (1,895 tracks), kind 3 with 32 bits (1,823), kind 1 (15) and kind 2 (33) with 32 bits (confirmed).

### Node vocabulary (demo headers)

| node | parent | contents | meaning |
| --- | --- | --- | --- |
| `ROOT` | | | root |
| `DEMO` | ROOT | | timeline |
| `CAMERA` | DEMO | `MOVE` + `CameraParam` | the demo camera |
| `MOVE` | CAMERA | unit `Transform` (0x961C702E): rotation, translation; `TARGET_NAME demo_camera` | camera transform (bound by 0x79A740, confirmed) |
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
| `MTP_LIST`, `MTP_PARENT_LIST` | ROOT | name tables (u32 count, then u32 hash + u32 string offset from the entry) | motion point names and parent bones (`SKL_023_RHAND`, `SKL_013_LHAND`), ending only |

Actors are named by `TARGET_NAME` and bound through the DemoData in the fox2 files (`../demo.md`, section Actors): `demo_camera`; `HM_plr0_main0_def` (the player; empty modelFiles path, DemoControlCharacterDesc `Player`, DemoModelDataNode part `Body`); `HM_och0_main0_def` (Lisa, "Ocho"; loaded by the demo from modelFiles + helpBoneFiles), `HM_coc0_main0_defN` (cockroaches), `HM_shl0_main0_def` (flashlight); `ENV_*` static models (locatorTypes 2, fmdl from modelFiles); `Eff_Null_*` effect nulls (locatorTypes 0). Bone units resolve against the fmdl bone names (`tools/fsm.py` loads them from the `tools/fmdl.py` JSON).

## Packet type 0: motion block

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | 0 |
| 0x04 | u32 | flags: bit 0 an event packet (type 2) with the same time follows (likely), bit 1 per-track flag table present, bit 2 offset vector present (0xAAC280, 0xA93970, confirmed) |
| 0x08 | u32 | start frame |
| 0x0C | u32 | frame count (31 for the first block of a copy, then 30; 300 in timeline demos; last block shorter) |
| 0x10 | u32 | track count n (all tracks of the header) |
| 0x14 | u32 | payload size (chunk size - 16) |
| 0x18 | u32 | 0, or the byte size of this packet plus the event packet of the same time (timeline demos and gc_p04_280/290; likely a prefetch size, not read by 0xA93970) |
| 0x1C | u32[n] | track data offset from the payload start, 0 = no data |
| | f32[3] | offset vector (flag bit 2); (0, 1, 0) in the streams that have one, except the endings (gc_p06_010_final in both languages), whose 33 whole-metre vectors follow the shots ((0, 1, -6) at the start, (-1, 1, 20), (0, 3, 10) and so on) |
| | u16[n] | track flags (flag bit 1) |
| | | track data: byte-aligned key streams |

Track flags (0xAAC280, confirmed):

| bit | meaning |
| --- | --- |
| 0 | static: one key |
| 1 | on the first track of a unit list (one model): the model has no data in this block; the runtime marks the model and skips all its tracks (0xAAC280 sets runtime byte +2 bit 0) |
| 2 | add the offset vector to the decoded values (sets channel flag bit 0 at 0xAAC280; data: all translation tracks with this flag decode to the DemoData and DemoControlCharacterDesc values only with the (0, 1, 0) offset added). The channels add the vector the stream state holds at +0x180 when they are evaluated (0xA933A0 hands +0x180 to 0xAAABC0), and 0xA93970 stores a packet's vector there only when the packet has flag bit 2, so a packet without a vector uses the last one an earlier packet stored (confirmed). Only the endings mix the two: 24 of their blocks have no vector (mostly one-frame blocks between 60-frame ones: 60, 2474, 3918, 4108, 4713, 5013, 5731, 6872 and their copies), and their flagged tracks are relative to the vector before them. Read without it, the camera and every flagged translation jumped by that vector for the block's frame (demo frames 61, 2475, 4109, 5014 and 5732, 5 to 21 m) and the camera kept the wrong value through the 60 frames of 6873, which only keep the previous state (track flags 13) |
| 3 | not evaluated in this block: the channel keeps its previous state |

Observed combinations: 0, 1, 3, 4, 5, 7, 9, 13 (confirmed). A block spans frames `start` to `start + count` inclusive: its last key sits on the next block's first frame and equals that block's first key (confirmed on all streams).

## Key streams

A track's data is a little-endian bit stream (the runtime reads 32-bit windows from 16-bit little-endian words, which is the same bit order). Layout: key 0, then pairs (u8 delta frames, key) until the deltas add up to the block's frame count; static tracks hold key 0 only (0xAD5D70, 0xAD46F0, confirmed). Keys sit at frames 0, d0, d0+d1, ...; values between keys are interpolated, the channel holds `time` and `1/duration` with `duration = delta * 5` ticks.

Rotation key (kinds 0 and 5), `3*b + 3` bits (0xAD6980, confirmed):

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

With b = 18 a key is 57 bits; a static rotation takes 8 bytes. For b > 16 the reader sign-extends each field and adds 2^b back, so the fields are unsigned.

Vector key (kinds 1-4, 6): `components * b` bits, each component an IEEE float (b = 32) or a 16-bit float with exponent bias 8 instead of the IEEE half's 15 (b = 16: sign << 31 | ((exponent << 13) + 0x3B800000 for the exponent and mantissa bits); exponent 0 decodes to 0, no infinities) (0xAD4530, 0xAD46F0, 0xAD4610, confirmed; with the IEEE bias the player's right start clip and one-frame pose came out 128 times too small).

Interpolation: vectors linear; rotations spherical with the shortest arc (a rotation channel sampler at 0xAB8F70 reads q0 at channel +0x20, q1 at +0x30, t = time (+0x0C) x 1/duration (+0x10), negates one quaternion when the dot product is negative and uses arccos of the dot product, confirmed; the kind 0 sampler of the main pose code was not traced, `tools/fsm.py` uses slerp for both kinds). End of a track: units with loop flag 0 hold the last key (0xAD5E70 `param_6`), loop flag 1 wraps to key 0 and accumulates a root delta (confirmed).

## Skeleton pose

Bone channels are local to the parent bone. The fmdl skeleton has no bind rotations (`fmdl.md`), so a rotation channel is the bone's local rotation. Translation channels are offsets added to the bind local translation (fmdl world minus parent world) (likely, from the data): every non-root translation channel of the player and Lisa skeletons is exactly 0 in all blocks, the ending's facial bones move by at most 1 cm against bind lengths of 2 to 4 cm, and the root bone (`SKL_000_WAIST`, bind position 0) carries the hip height (1.04 m in gc_p00_010, 1.26 m for Lisa in gc_p07_030). Bones without a channel keep the bind pose. The model root channel (`MOTION/MOVE`, unit 0x01AD535D) places the whole model in demo space: `model = demo transform x root x bone chain`.

## Camera

| channel | source | notes |
| --- | --- | --- |
| position, rotation | `CAMERA/MOVE` | demo space; forward is the camera's local -Z, up local +Y (checked against the start room door, `validation.txt`) |
| focal length | `CameraParam` float (13, 18.8, 20, 35, 48 mm in the data) | written to Camera.focalLength (+0x34) by 0x7991D0 through 0xA933A0; the film gate is 24 x 13.5 mm (0x798420 uses atan2(12, f) and atan2(6.75, f)): vertical FOV = 2 atan(6.75 / f), horizontal = 2 atan(12 / f) (confirmed) |
| near clip | 0.05 m | set when the demo camera is created (0x798D40 writes 0x3D4CCCCD to Camera +0x40, confirmed) |
| far clip, exposure, bloom, shutter | copied from the game camera at start (0x798D40) and animated by ExecCommand functors |
| focus distance, aperture (depth of field) | ExecCommand functors only (table in `../demo.md`); no DOF track in the streams |

The first camera key equals DemoData `cameraStartTranslation`/`cameraStartRotation` within a few mm and the last key equals `cameraTranslation`/`cameraRotation` exactly (all camera demos, `summary.txt`).

## Packet type 2: events

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | 2 |
| 0x04 | u32 | payload size |
| 0x08 | u32 | 0 |
| 0x0C | u32 | event track hash 0xC8F4FCB6 (the default track; 0xAA21B0 substitutes it for an empty name) |
| 0x10 | u32 | event count (read as u16) |
| 0x14 | u32[count] | record offsets from +0x0C |

Record (0xACC3A0, 0xACED90, 0xB2C230, confirmed):

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | event type (StrCode32) |
| 0x04 | u8 | bits 0-5 section count, bits 6-7 section size: 0 two i32, 1 two i16, 2 two i8 |
| 0x05 | u8 | int count |
| 0x06 | u8 | float count |
| 0x07 | u8 | string count |
| 0x08 | | sections (start frame, end frame); i32 form: -1 = no end (instant event), bit 30 = boundary outside this block, cleared on read (the event continues from or into a neighbour block) |
| align 4 | u32[ints] | ints (the runtime copies at most 48) |
| | f32[floats] | floats (at most 32) |
| | u64[strings] | StrCode64 values in the low 48 bits (at most 16) |

Event types (StrCode32 of the names, confirmed by hash):

| type | hash | payload | notes |
| --- | --- | --- | --- |
| `DemoStart` | 0xD3185DFF | int (0 and 1, two events at frame 0) | |
| `CreateCamera` / `DeleteCamera` | 0x7F4D8E71 / 0xF4603510 | int 2 | frame 0 / L |
| `CreateModel` / `DeleteModel` | 0xC5806267 / 0x75586EE7 | ints (1, 0) static or (0, 1) skinned model, strings (model name, fmdl path, then 4 empty for skinned) / string (model name) | |
| `CreateLocator` / `DeleteLocator` | 0x966D8EC3 / 0x3148F136 | string (locator name) | |
| `VisibleModel` | 0x3E09E7B9 | int visible, string model | |
| `VisibleMesh` | 0x6B491E18 | int visible, strings model, mesh | |
| `ClipEnd` | 0x2A36C514 | int | at L - 1; 0xB20960 calls 0xB1B5B0 on the clock |
| `DemoEnd` | 0xD2A6999A | none | at L; 0xB20960 calls 0xB1B550 (end frame) |
| `ExecCommand` | 0x2379C011 | functor call (below) | 7,637 records in the 41 streams (all copies) |

Priorities used when events are cached (0xB12560): ClipEnd, DemoEnd, DemoStart 200; CreateCamera, CreateModel, CreateLocator 300; DeleteLocator, DeleteModel, DeleteCamera 400; VisibleModel, VisibleMesh 500; ExecCommand by functor (500 or 600, or the value set with 0xB078F0).

### ExecCommand

The source form is visible in the two event files (`.evf`, fox2 `EventDataUnit` entities with `eventName ExecCommand`, `paramString`, `paramInt`, `paramFloat` and a `TimeSection`): the compiled record stores `paramInt`, `paramFloat` and StrCode64 of `paramString` in the same order (confirmed by comparing `gc_p04_270_snd.evf` with the same functors in the streams).

Strings: the parameter values, then 5 fixed strings: clip reference (`konShotCameraN;start;end;offset`), empty, `DemoEvent_<functor>`, `<functor>`, category (`<group>;<n>;<name>;`). The runtime looks up the handler with the second-to-last string (0xB12560 reads `strings[count - 2]`, confirmed).

Ints: raw int values, then one descriptor pair per parameter, then a footer (layout likely: the grammar below parses all 7,637 records; the parser code was not traced):

| word | meaning |
| --- | --- |
| raw ints | values of int, bool and enum parameters |
| (u32 key, u32 type << 16 \| index) per parameter | key = StrCode32 of the parameter name; index into the value array of the type |
| plain footer: (3 \| n << 16), (2 \| 1 << 16) | n plain parameters |
| interpolated footer: (c \| k << 16 \| 1 << 24), (4 \| m << 16), (2 \| 2 << 16) | the first k descriptors are interpolated between the section start and end and store start and end values back to back; m plain ones follow; c unknown (4, 6, 8, 14, 16) |
| length footer: (5 \| n << 16), -1, frame, (4 \| 1 << 16) | the 4 skip events (flag 2): n parameters (Wwise event id ints in gc_p02_100 and gc_p07_030, strings in gc_p04_280) and the frame they wait for (2278, 375, 1307, 531) |
| last: (6 \| flags << 16) | flags 2: skip event, `ints[count - 3]` is the frame it waits for (0xB2C560 stores it as the event's frame; `../demo.md` 11); 8: interpolated start values from the functor's start getter; 0x10: end values from its end getter (0xB2CD90; `../demo.md` 10) (confirmed) |

Parameter types (high 16 bits): 0x0B string, 0x16 string (path), 0x08 float, 0x0A bool, 0x05 int (Wwise ids), 0x04 int, 0x13 color (4 floats: RGBA), 0x0F vector4, 0x10 vector4, 0x0E, 0x020E, 0x040E vector3, 0x0210 quaternion, 0x14 file (index into the strings; DemoUiFunctor_Create) (confirmed against the evf source values; 0x14 from its index).

Handlers are registered per functor StrCode64 with 0xB07550 (283 registrations in 72 functions; 90 functors are used by P.T. streams); the list with parameters and effects is in `../demo.md`. Demo messages are `ExecCommand` events with functor `DemoSendMessageFunctor` (0xCEE2EDD67199) and a string parameter `message` (StrCode32 0x73F3ECE4) plus an optional string parameter 0x26C8E5B5 (message body, empty in all P.T. data).

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
| gc_p04_290 | 365 | StrCode64 0xAA36200CF879 (unresolved; no ShDemoScript listens to it) |
| gc_p05_010 | 3594 | FinishMotion |
| gc_p07_030 | 1306 | GotoGameOver |

Later copies repeat the messages at +L and +2L. `Play`, `Start`, `PlayInit`, `Finish`, `PlayEnd`, `Interrupt`, `Skip` and a second `FinishMotion` are generated by the runtime (`../demo.md`); 0x76E760 drops data messages named Play, PlayEnd, Finish, Interrupt, Skip or Start (confirmed).

## Output of tools/fsm.py --json

`dump/demo/<name>.json`: chunk counts, `length`, `loops`, `demoData` (from `dump/fox2`), `tree` (nodes with params), `actors` (kind, target, units, binding from DemoData, blocks where the actor is inactive), `tracks` (index, actor, unit or bone name, kind, bits, flag counts, decoded keys of the first copy), `blocks`, `camera.frames` (frame, position, quaternion, focal length per frame), `transforms` (per frame for locators, model roots, SI Frame, timeline), `skinned` (per frame, per bone quaternion and translation offset as stored; null where the bone has no channel, which means bind pose; add the offset to the fmdl bind local translation), `events` (first copy; ExecCommand decoded into functor, params, interpolation, length). `summary.txt` lists every demo; `validation.txt` maps camera and player paths into level coordinates through the demo center locators.
