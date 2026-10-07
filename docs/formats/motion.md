# Motion files (.mtar, gani, .frig, .frdv)

Animations outside the demo streams: gimmick and player motion archives, rigs and help-bone driver files. The animation
data uses the same node tree, units table and key codec as the demo streams (`fsm.md`); only the container and a few
node kinds differ. Tool: `tools/motion.py` (lists archives, decodes animations to `<dir>/motion_<key>.json` with
`--json`, prints `.frig` and `.frdv` contents, writes help bone reference poses with `--help-poses`).

## Files

| file | package | contents |
| --- | --- | --- |
| `/Assets/sh/motion/mtar/gimmick/ShGimmick_layers.mtar` | resident.fpk | 11 gimmick animations (motion keys from `ShGimmickSetUp.lua`) |
| `/Assets/sh/motion/mtar/player/ShPlayer_layers.mtar` | player2_common_motion.fpk | 14 player animations (names not in any file) |
| `/Assets/sh/motion/motion_graph/player/ShPlayer_layers.mog` | player2_common_motion.fpk | motion graph, magic `FOXMOTIONGRAPH` (partly decoded) |
| `/Assets/sh/rig/frig/human_finger.frig` | resident, ending, hallway, plparts_normal | rig "HumanFinger": the humanoid body rig with fingers, `gameRigFile` of Lisa's and the player's parts |
| `/Assets/sh/chara/*/Scenes/*.frdv` | resident, plparts_normal | help-bone drivers for bab0, coc0, dol0, hsh0, och0, plr0 |

`Gimmick.AddMotionPath{key, path}` in `ShGimmickSetUp.lua` maps motion keys to gani paths, and the archive entry for a
path is its PathCode64 (`textures.md`).

## .mtar

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | magic 0x0C012B72 |
| 0x04 | u32 | animation count |
| 0x08 | u16 | 0x17 (gimmick), 0x12 (player); unknown |
| 0x0A | u16 | 0x38; unknown |
| 0x0C | 20 bytes | 0 |
| 0x20 | 16 bytes x count | entries: u64 PathCode64 of the gani path, u32 offset, u32 size |

Gimmick entries:

| motion key | gani | frames | units | tracks | notes |
| --- | --- | --- | --- | --- | --- |
| Baby | bab0/bab0_m00_020 | 719 | 23 | 31 | 22 bones + root |
| Baby3 | bab0/bab0_m00_030 | 2340 | 23 | 31 | |
| Baby4 | bab0/bab0_m00_040 | 1750 | 23 | 31 | |
| Ocho | och0/och0_m01_011 | 1800 | 18 | 56 | rig driven (below) |
| OchoStop | och0/och0_m01_021 | 1800 | 18 | 56 | rig driven |
| OchoDash | och0/och0_m01_012 | 120 | 18 | 56 | rig driven |
| CeilLamp | lte0/lte0_m00_010 | 1119 | 4 | 5 | 3 bones + root |
| CeilLampStrong | lte0/lte0_m00_020 | 440 | 4 | 5 | |
| Freezer | frz0/frz0_m00_010 | 299 | 4 | 5 | |
| FreezerStrong | frz0/frz0_m00_020 | 2200 | 4 | 5 | |
| BagTalk | pab0/pab0_m00_010 | 1630 | 5 | 7 | |

(paths are `/Assets/sh/motion/SI_game/fani/bodies/<model>/<name>.gani`)

## gani

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | magic 0x0BFCA2D2 |
| 0x04 | u32 | header size 0x20 |
| 0x08 | u32 | gani size |
| 0x0C | u32 | 0 |
| 0x10 | 16 bytes | 0 |
| 0x20 | | node tree (`fsm.md` node layout; gani nodes are unnamed: name offset 0, data at node + 0x30) |

Tree:

```
ROOT
  MOTION                params SLOPE_ANGLE, SLOPE_DIR (float, keys without strings)
    SKL_LIST            (0x91E4534B) name table: u32 count, (u32 StrCode32, u32 string offset from the entry); bone names
    UNIT                (0xC6E937B9) data type 1: units table with inline track data
    0x1622762D          data type 3: event set (optional)
    MTP, MTP_LIST, MTP_PARENT_LIST   motion points (one player animation)
```

UNIT: the units table of `fsm.md` (unit count, track count, 0x01000000 or 0x01000001, frame count, 5 ticks per frame,
unit offsets). Track entries carry the data offset relative to the entry itself. Units:

- `0x01AD535D` (the model root; the same unit as a demo stream's model `MOVE`): kind 5 rotation (slerp, 15 bits) and
  kind 6 root translation (3 floats), unit flags 5 (loop + static), 4 (static) in BagTalk, Baby3 and Baby4.
- bones: StrCode32 of the names in SKL_LIST (`SKL_000_ROOT`, `SKL_001_BODY`, ...), kind 0 rotations with 13 bits and kind
  3 translations with 32-bit floats, unit flags 1 (loop) or 5 (loop + static).

Unit flags: bit 0 loop (at the end wrap to key 0 and accumulate the root delta; demo units never set it), bit 1 cubic
vector interpolation, bit 2 static (one key per track). Whether a clip loops is decided by bit 0 of the first unit's
flags unless the controller forces it: BagTalk, Baby3, Baby4 and OchoDash stop at their last frame and hold it, the
other seven gimmick motions loop.

Rig driven animations (UNIT field 0x01000001; Ocho, OchoStop, OchoDash and all 14 player animations): 18 units in the
order of the units of the HumanFinger rig, named `RIG_ROOT` (0xD3C3FF90; slerp rotation 12 bits + root translation) and
`RIG_` + the name of the unit's first joint (`RIG_SKL_000_WAIST` 0xA8CE0F57, `RIG_SKL_001_SPINE`, `RIG_SKL_002_CHEST`,
`RIG_SKL_003_NECK`, `RIG_SKL_004_HEAD`, `RIG_SKL_010_LSHLD`, `RIG_SKL_013_LHAND`, `RIG_SKL_020_RSHLD`, `RIG_SKL_023_RHAND`,
`RIG_SKL_030_LTHIGH`, `RIG_SKL_032_LFOOT`, `RIG_SKL_040_RTHIGH`, `RIG_SKL_042_RFOOT`, `RIG_SKL_033_LTOE`, `RIG_SKL_043_RTOE`,
`RIG_SKL_101_LF10`, `RIG_SKL_201_RF10`, the last two 16 finger rotations each). The channels are rig space values, not
bone local rotations (Rig evaluation). `tools/motion.py` names the units and writes the evaluated poses (`rig` in
`motion_<key>.json`).

Keys: identical to demo tracks: rotation keys `3*b + 3` bits (axis on the octant + 3 sign bits + half angle), vectors
as floats or halves, u8 frame deltas, LSB-first bit stream, slerp/lerp between keys (`fsm.md`). The gimmick animations
store a key on every frame except static tracks.

### Events

Node 0x1622762D (data type 3): a set `u32 hash (0x0BFE2CF6), u16 count, u16 0, i32 offsets[count]` of event tables with
the demo event table layout (`u32 track hash, u16 count, u16 0, i32 offsets[count]`, records as in `fsm.md` "Packet
type 2"). In the gimmick animations the records are sound triggers: Freezer type 0x0F085179 at frames 5 and 155 with
ints (2, 0x951EF647 = a `sfx_common` Wwise event), CeilLamp types 0x37A8C200 / 0x2B112F42 at the swing extremes, BagTalk
type 0xA5F103BC (event type names unresolved).

## Decoded example (Freezer, CeilLamp)

`python tools/motion.py --json <dir> --only Freezer CeilLamp` writes `motion_Freezer.json` and `motion_CeilLamp.json`
(per unit: flags, tracks with keys; `perFrame` rows of quaternion + translation per bone; events):

| motion | bone | content |
| --- | --- | --- |
| Freezer (299 frames, loops) | root, SKL_000_ROOT | static identity |
| | SKL_001_BODY, SKL_002_BODY | per-frame sway of about 3 degrees, frame 299 equals frame 0 |
| CeilLamp (1119 frames, loops) | SKL_000_ROOT | constant tilt about Z (1.5 degrees) |
| | SKL_001_BODY | yaw swing between +60 and -60 degrees |
| CeilLampStrong (440 frames) | SKL_000_ROOT | swing of +-15 degrees |
| | SKL_001_BODY | full turn per cycle |

Translations are not animated in these three models; bone positions come from the fmdl bind pose (`fmdl.md`: local =
world - parent world, no bind rotations), so the pose is `local translation (fmdl) + rotation (gani)` per bone. Where
gani bones have translation channels (Baby: `SKL_001_CHEST` about 2 mm) they are offsets added to the bind local
translation, as in the demo streams.

## .frig

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | magic 0x21EA256C |
| 0x04 | u32 | offset of the name (0x68) |
| 0x08 | u32 | 0x66 (unknown) |
| 0x0C | u32 | unit count (18) |
| 0x10 | u32 | track count (56, the gani track count) |
| 0x14 | u32 | file size |
| 0x18 | u32 | joint table offset (0x73C) |
| 0x1C | u32 | mask table offset (0x340) |
| 0x20 | u32[count] | unit offsets |
| 0x68 | char[16] | rig name `HumanFinger` |

Unit header (every type): u32 type, u16 track count, u16 joint count, i16 parent joint, i16 parent unit. Joints index
the joint table, tracks are gani track indices. Field positions per type:

| type | HumanFinger units | layout | channels (track kind) |
| --- | --- | --- | --- |
| 1 root | RIG_ROOT | +0x10 rotation track, +0x12 translation track | rotation (5), translation (6) |
| 7 waist | SKL_000_WAIST | +0x10 joint, +0x12 rotation track, +0x14 translation track | rotation (0), vector (3) |
| 2 rotation | spine, chest, neck, head, hands, feet | +0x10 joint, +0x12 track | rotation (0) |
| 4 local rotation | toes | +0x10 joint, +0x12 track | rotation (0) |
| 11 chain | fingers (16 joints) | +0x10 first joint, +0x12 first track, counts in the header | rotation (0) per joint |
| 3 leg | thighs | +0x20 hinge axis (1, 0, 0), +0x30 thigh and knee joints, +0x34 target and swivel tracks, +0x38 foot joint | vector (3), rotation (0) |
| 8 arm | shoulders | +0x20 hinge axis (0, -1, 0) left, (0, 1, 0) right, +0x30 clavicle, upper arm and forearm joints, +0x36 clavicle rotation, target and swivel tracks, +0x3C hand joint | rotation, vector, rotation |

Types 5, 6, 9 and 10 exist in the game but no P.T. rig uses them.

Joint table: u32 count (53), then (u32 unit, u32 StrCode32 of the bone name) per joint. Joints 0..52 are
`SKL_000_WAIST` .. `SKL_216_RF53`, the first 53 bones of both och0 (90 bones) and plr0 (191 bones) in the same order.

Masks: u32 unit count (18), u32 mask count (11), u32 offsets from the table start; each mask is u32 StrCode32 of the
name, char[12] name, one float weight per unit: Lower, Upper, Head, LArm, RArm, LHand, RHand, MirrorL, MirrorR,
CarryUpper, CarryLArm (layer weights for partial-body motion layers, likely; not needed for single-layer playback).

## Rig evaluation

Pipeline for rig driven motions:

1. Sampling: each unit reads its tracks. The IK targets (vector channels of types 3 and 8) are stored in motion space
   and moved into root space with the RIG_ROOT channels: `v' = R_root^T (v - p_root)`. The waist position and all
   rotations are already root space.
2. Evaluation, units in order; the results are model space rotations R and positions P per bone:
   - type 1: nothing (root motion is used by the owner).
   - type 7: R = rotation channel, P = vector channel.
   - type 2: R = channel (model space, not relative to the parent); P = P_parent + R_parent * bind local.
   - types 4 and 11: R = R_parent * channel; P as type 2.
   - type 3: hip H = P_parent + R_parent * local(thigh); target T = P_parent + v; swivel q = R_parent * channel; pole
     a = q * (1, 0, 0). With d = T - H, D = |d|, L1 = |local(knee)|, L2 = |local(foot)|: e = normalize(d x (a x d));
     x = max((D^2 + L1^2 - L2^2) / 2D, 0) when D < L1 + L2, else x = L1; h = sqrt(max(L1^2 - x^2, 0)); knee offset
     k = e h + (d / D) x; hinge b = e x (d / D). R_thigh = b u^T + k' t^T + (k' x b)(t x u)^T with u the unit axis,
     k' = k / |k|, t = local(knee) / L1; R_knee is built the same way from s = normalize(d - k) and local(foot) / L2.
     P_thigh = H, P_knee = H + R_thigh * local(knee). The foot is its own type 2 unit and lands on T when the target
     is reachable.
   - type 8: the clavicle as type 2 from channel 0; the chain from the upper arm (P_clavicle + R_clavicle * local(upper
     arm)) to T = P_chest + v, pole = channel 2 * (1, 0, 0), hinge = the unit axis, as type 3.
   - afterwards every bone the rig did not set takes its parent rotation and the position P_parent + R_parent * bind
     local; help bones are then driven by the .frdv (below).
3. Looping rig motions wrap to frame 0 at the end like the other gani units.

Ocho frame 0 (root space): waist (0, 1.276, 0), foot targets (0.125, 0.109, -0.063) and (-0.201, 0.180, 0.085), leg
swivels about -90 degrees around Y (pole = forward, knees bend forward), hands at 1.12 to 1.14 m.

The Python model (`tools/motion.py`, `evaluate_rig`) was compared with the game's own evaluation functions on every
frame of the 17 rig driven motions and 3000 random poses: they agree within 0.17 mm.

## Root motion of gimmick bodies

Gani frames are 5 ticks and the body clock advances 299.7003 ticks a second, so gimmick motions run at 59.94 frames
per second (60000/1001). Each frame the RIG_ROOT delta between consecutive frames (carried across the loop seam) is
composed onto the body's node after the gimmick logic has placed it, in every logic state and while hidden. For Lisa
that means: Ocho travels 1.613 m forward over its 1800-frame loop with 0.26 m of sideways sway; OchoStop has no net
travel, a sway of 0.10 m with a period near 106 frames; OchoDash's root is constant.

## .frdv (help bones)

File: `FRDV`, u32 0x0BFFB0A8, u32 entry count, u32 0, u32 entry offsets, 0x80-byte entries. Demos with their own skinned
models list them as `helpBoneFiles` (keyed by the model name) in the DemoStreamAnimation, and the parts' ModelDescription
names one as `helpBoneFile` (och0, bab0, plr0). P.T. ships six: och0, plr0, dol0 and hsh0 with 21 entries each (the
same drivers on the same bone indices), bab0 (3 entries: the heart) and coc0 (12: the legs).

Entry:

| offset | type | field |
| --- | --- | --- |
| 0x00 | u16 | type (1 to 0x16; 0x10 to 0x16 are not used by P.T.) |
| 0x02 | i16 | driven bone |
| 0x04 | i16 | source bone |
| 0x06 | i16 | not read by the types P.T. uses |
| 0x08 | i16 | parent bone: base of the driven rotation and position |
| 0x0A | i16 | reference bone, -1 for none: the source rotation is taken as conj(R_ref) R_src |
| 0x10 | f32 | weight |
| 0x18, 0x1C | f32 | lower and upper limit (degrees) |
| 0x20 | u32 | axis index: types 11 and 13 turn about x, y or z, type 1 slides that component |
| 0x24 | f32 | slide factor (types 12, 13) |
| 0x2C, 0x30 | f32 | slide limits (decimetres) |
| 0x34 | u32 | slid component of the bind offset (types 12, 13) |
| 0x40 | vec4 | axis a |
| 0x50 | vec4 | axis b |

The help bones work on the model space rotations and positions after the rig, entries in file order, each writing only
its driven bone: R_d = R_p r and P_d = P_p + R_p o, where o is the driven bone's bind offset (the fmdl local position)
with one component possibly replaced. With q = R_src or conj(R_ref) R_src:

- 1 (bab0's heart): r = identity; o[axis] = 0.1 limit(weight x 114.59155 x acos(|q.w|)), the angle of q in degrees.
- 2: r = slerp(identity, q, weight).
- 7: twist of q about a (q with the shortest arc from a to q a q* taken off), r = slerp(identity, twist, weight); a
  negative weight gives the conjugate of the slerp with -weight.
- 12: as 7, and o[slide axis] = 0.1 limit(slide x (b . q a q*), slide min, slide max).
- 11: with v = q a q*, theta = 2 acos(sqrt((1 + a . v) / 2)), x = b . v, y = -(a x b) . v and
  g = sign(x) (2 / pi) atan2(|x|, |y| + 1e-10), r turns about the axis index by limit(weight x g x theta) with the
  limits in degrees.
- 13: as 11 with g = 1 - (2 / pi) atan2(|x|, |y| + 1e-10) for y >= 0 and (2 / pi) atan2(...) - 1 below, plus the slide
  of 12.

The slerp takes the shorter way round, with linear weights from |dot| >= 0.999, normalized; the limits clamp. At rest
every entry reproduces its bind offset, so the slides are authored in decimetres. Only coc0 has help bones under help
bones, and their entries come after their parents'. `tools/motion.py <frdv files> --json <dir> --help-poses N` writes
`helpbones_<model>.json` with N random poses and their help bones, which the port's unit test compares against.

## .mog (player motion graph)

`ShPlayer_layers.mog` (4016 bytes, magic `FOXMOTIONGRAPH`) is the player's motion graph. Its tail holds 48-bit
StrCode64 names (`Demo`, `init`, `Full`, `Null` and unresolved ones), the five node ids the locomotion code requests
and the 14 gani PathCode64s. Records before it use self-relative offsets: 0x48-byte nodes with a name and a clip (the
100-frame idle; the walk loops +Z, +X, -Z, -X; the four 40-frame start clips and the four 40-frame stop clips), and 16
transition records of 0x28 bytes. Transition endpoints and conditions are not decoded.

## Shared codec

The demo stream tracks and gani tracks use the same decoders. Differences: gani data is inline (entry-relative offsets)
and one block covers the whole animation; demo data comes per streamed block with per-track flags and the offset
vector; gani units loop, demo units hold the last key.
