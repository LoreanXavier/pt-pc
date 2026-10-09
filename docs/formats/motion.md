# Motion files (.mtar, gani, .frig, .frdv)

Animations outside the demo streams: gimmick and player motion archives, rigs and help-bone driver files. The animation data uses the same node tree, units table and key codec as the demo streams (`fsm.md`, sections "Packet type 1" and "Key streams"); only the container and a few node kinds differ. Tool: `tools/motion.py` (lists archives, decodes animations to `dump/demo/motion_<key>.json`, prints `.frig` and `.frdv` contents, writes help bone reference poses with `--help-poses`). Marks as in `fsm.md`.

## Files

| file | package | contents |
| --- | --- | --- |
| `/Assets/sh/motion/mtar/gimmick/ShGimmick_layers.mtar` | resident.fpk | 11 gimmick animations (motion keys from `ShGimmickSetUp.lua`) |
| `/Assets/sh/motion/mtar/player/ShPlayer_layers.mtar` | player2_common_motion.fpk | 14 player animations (names not in any file) |
| `/Assets/sh/motion/motion_graph/player/ShPlayer_layers.mog` | player2_common_motion.fpk | motion graph, magic `FOXMOTIONGRAPH` (not decoded) |
| `/Assets/sh/rig/frig/human_finger.frig` | resident, ending, hallway, plparts_normal | rig "HumanFinger": the humanoid body rig with fingers, `gameRigFile` of `och0_main0_def.parts` and `plr0_main0_def_v00.parts` (confirmed) |
| `/Assets/sh/chara/*/Scenes/*.frdv` | resident, plparts_normal | help-bone drivers for bab0, coc0, dol0, hsh0, och0, plr0 |

The gimmick archive is referenced by `resident_common_gimmick.fox2` (ShGimmick game object); `Gimmick.AddMotionPath{key, path}` maps motion keys to gani paths and the archive entry for a path is its PathCode64 (all 11 entries match `pathcode.raw_path_code64(path)`, confirmed).

## .mtar (P.T. uses the "type 1" layout)

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | magic 0x0C012B72 (checked at 0xAB13D1, 0xAB2781) |
| 0x04 | u32 | animation count |
| 0x08 | u16 | 0x17 (gimmick), 0x12 (player); unknown |
| 0x0A | u16 | 0x38; unknown |
| 0x0C | 20 bytes | 0 |
| 0x20 | 16 bytes x count | entries: u64 PathCode64 of the gani path, u32 offset, u32 size |

Gimmick entries (confirmed):

| motion key | gani | frames | units | tracks | notes |
| --- | --- | --- | --- | --- | --- |
| Baby | bab0/bab0_m00_020 | 719 | 23 | 31 | 22 bones (SKL_LIST) + root |
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
| 0x00 | u32 | magic 0x0BFCA2D2 (checked at 0xA96785, 0xA98061, 0xAB2DA4) |
| 0x04 | u32 | header size 0x20 |
| 0x08 | u32 | gani size |
| 0x0C | u32 | 0 |
| 0x10 | 16 bytes | 0 |
| 0x20 | | node tree (`fsm.md` node layout; gani nodes are unnamed: name offset 0, data at node + 0x30) |

Tree (confirmed on all 25 animations):

```
ROOT
  MOTION                params SLOPE_ANGLE, SLOPE_DIR (float, keys without strings; read by 0xAB0540)
    SKL_LIST            (0x91E4534B) name table: u32 count, (u32 StrCode32, u32 string offset from the entry); bone names
    UNIT                (0xC6E937B9) data type 1: units table with inline track data
    0x1622762D          data type 3: event set (optional)
    MTP, MTP_LIST, MTP_PARENT_LIST   motion points (one player animation)
```

UNIT: the units table of `fsm.md` (unit count, track count, 0x01000000 or 0x01000001, frame count, 5 ticks per frame, unit offsets). Track entries carry the data offset relative to the entry itself (0xAAC010 passes `entry + offset`, confirmed). Units:

- `0x01AD535D` (the model root; the same unit as a demo stream's model `MOVE`): kind 5 rotation (slerp, 15 bits) and kind 6 root translation (3 floats), unit flags 5 (loop + static), 4 (static) in BagTalk, Baby3 and Baby4.
- bones: StrCode32 of the names in SKL_LIST (`SKL_000_ROOT`, `SKL_001_BODY`, ...), kind 0 rotations with 13 bits and kind 3 translations with 32-bit floats, unit flags 1 (loop) or 5 (loop + static).

Unit flags (0xAAC010, 0xAAB550, confirmed): bit 0 loop (at the end wrap to key 0 and accumulate the root delta; demo units never set it), bit 1 cubic vector interpolation, bit 2 static (one key per track). The alternative loader 0xAAC120 reads per-track (bits, offset) words from a separate block (not used by these files).

Whether a clip loops: the layer advance (0xAA0990, called by 0xAA2760 with the controller's loop mode, motion controller +0x40 & 3) passes the mode to 0xAA7580 and 0xAAB220, where 1 forces a loop, 2 forbids it and 0 takes bit 0 of the first unit's flags (units table +0x14 -> unit +5) (confirmed); the gimmick bodies run with mode 0 (likely: their data carries both kinds of first unit, and the swings loop). At the end a looping clip wraps (node flags 0x12, loop count + 1); any other clip stops at its last frame and holds it (flags 0x46). The gimmick motions that do not loop are BagTalk, Baby3 and Baby4 (first unit, the root, flags 4) and OchoDash (RIG_ROOT flags 4); the other seven loop. The body's motion layer keeps its request after a clip ends (layer +0x4C & 3 stays 1, `gameplay.md` 9.3), so a held clip still counts as playing.

Rig driven animations (UNIT field 0x01000001; Ocho, OchoStop, OchoDash and all 14 player animations): 18 units in the order of the units of the HumanFinger rig (section .frig), named `RIG_ROOT` (0xD3C3FF90; slerp rotation 12 bits + root translation) and `RIG_` + the name of the unit's first joint: `RIG_SKL_000_WAIST` 0xA8CE0F57, `RIG_SKL_001_SPINE` 0x20176D01, `RIG_SKL_002_CHEST` 0x526E0EC1, `RIG_SKL_003_NECK` 0x00C28D10, `RIG_SKL_004_HEAD` 0x6D448D74, `RIG_SKL_010_LSHLD` 0xF288BFFE, `RIG_SKL_013_LHAND` 0x60ED6C59, `RIG_SKL_020_RSHLD` 0x7AFA9000, `RIG_SKL_023_RHAND` 0xFD19F0F6, `RIG_SKL_030_LTHIGH` 0x5E9CF7E6, `RIG_SKL_032_LFOOT` 0x8A2BA763, `RIG_SKL_040_RTHIGH` 0x72497575, `RIG_SKL_042_RFOOT` 0xDE542E60, `RIG_SKL_033_LTOE` 0x27351E54, `RIG_SKL_043_RTOE` 0xA03A6769, `RIG_SKL_101_LF10` 0xEA4D1B8B and `RIG_SKL_201_RF10` 0x8DAA7DB8 (16 finger rotations each) (StrCode32, all 17 confirmed by hash; only `SKL_004_HEAD` appears as a string in the eboot). The runtime does not look the unit hashes up: the rig units list gani track indices and 0xAA7BA0 builds the channel buffer from the rig (0xAAC7C0) instead of the SKL_LIST binding (0xAA7900) when the motion is rig driven (flag 0x100000) (confirmed). The channels are rig space values, not bone local rotations (section Rig evaluation). `tools/motion.py` names the units and writes the evaluated poses (`rig` in `motion_<key>.json`).

Keys: identical to demo tracks: rotation keys `3*b + 3` bits (axis on the octant + 3 sign bits + half angle), vectors as floats or halves, u8 frame deltas, LSB-first bit stream, slerp/lerp between keys (`fsm.md`). The gimmick animations store a key on every frame (Baby, CeilLamp, Freezer bones: `frames + 1` keys) except static tracks.

### Events

Node 0x1622762D (data type 3, read by 0xAB2270): a set `u32 hash (0x0BFE2CF6), u16 count, u16 0, i32 offsets[count]` of event tables with the demo event table layout (`u32 track hash, u16 count, u16 0, i32 offsets[count]`, records as in `fsm.md` "Packet type 2"). 0xAB2270 iterates the tables and dispatches each through 0xA9B3D0 by the table hash (confirmed). In the gimmick animations the records are sound triggers: Freezer type 0x0F085179 at frames 5 and 155 with ints (2, 0x951EF647 = a `sfx_common` Wwise event), CeilLamp types 0x37A8C200 / 0x2B112F42 at the swing extremes (frames 174, 446, 723, 1002 / 6, 314, 579, 864), BagTalk type 0xA5F103BC (likely; event type names unresolved).

## Decoded example (Freezer, CeilLamp)

`python tools/motion.py --json dump/demo --only Freezer CeilLamp` writes `motion_Freezer.json` and `motion_CeilLamp.json` (per unit: flags, tracks with keys; `perFrame` rows of quaternion + translation per bone; events):

| motion | bone | content |
| --- | --- | --- |
| Freezer (299 frames, loops) | root, SKL_000_ROOT | static identity |
| | SKL_001_BODY, SKL_002_BODY | per-frame sway of about 3 degrees (e.g. frame 0 (0.0087, 0.0025, 0.0261, 0.9996)), frame 299 equals frame 0 |
| CeilLamp (1119 frames, loops) | SKL_000_ROOT | constant tilt about Z (0.013 quaternion, 1.5 degrees) |
| | SKL_001_BODY | yaw swing between +60 and -60 degrees (frame 0 (0, 0.4999, 0, 0.8661), frame 559 (0, -0.4999, 0, 0.8661)) |
| CeilLampStrong (440 frames) | SKL_000_ROOT | swing of +-15 degrees |
| | SKL_001_BODY | full turn per cycle |

Translations are not animated in these three models; bone positions come from the fmdl bind pose (`fmdl.md`: local = world - parent world, no bind rotations), so the pose is `local translation (fmdl) + rotation (gani)` per bone. Where gani bones have translation channels (Baby: `SKL_001_CHEST` about 2 mm) they are offsets added to the bind local translation, as in the demo streams (`fsm.md` section Skeleton pose; likely).

## .frig

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | magic 0x21EA256C (no loader check found; the value is not an immediate in the eboot) |
| 0x04 | u32 | offset of the name (0x68) |
| 0x08 | u32 | 0x66 (unknown) |
| 0x0C | u32 | unit count (18) |
| 0x10 | u32 | track count (56, the gani track count; 0xAAC7C0 sizes the channel buffer with it, confirmed) |
| 0x14 | u32 | file size |
| 0x18 | u32 | joint table offset (0x73C) |
| 0x1C | u32 | mask table offset (0x340) |
| 0x20 | u32[count] | unit offsets |
| 0x68 | char[16] | rig name `HumanFinger` |

Unit header (every type): u32 type, u16 track count, u16 joint count, i16 parent joint, i16 parent unit (confirmed from the data and 0xABF270). Joints index the joint table, tracks are gani track indices. Field positions per type (confirmed by the evaluators, samplers and 0xABF270):

| type | HumanFinger units | layout | channels (track kind) |
| --- | --- | --- | --- |
| 1 root | RIG_ROOT | +0x10 rotation track, +0x12 translation track | rotation (5), translation (6) |
| 7 waist | SKL_000_WAIST | +0x10 joint, +0x12 rotation track, +0x14 translation track | rotation (0), vector (3) |
| 2 rotation | spine, chest, neck, head, hands, feet | +0x10 joint, +0x12 track | rotation (0) |
| 4 local rotation | toes | +0x10 joint, +0x12 track | rotation (0) |
| 11 chain | fingers (16 joints) | +0x10 first joint, +0x12 first track, counts in the header | rotation (0) per joint |
| 3 leg | thighs | +0x20 hinge axis (1, 0, 0), +0x30 thigh and knee joints, +0x34 target and swivel tracks, +0x38 foot joint | vector (3), rotation (0) |
| 8 arm | shoulders | +0x20 hinge axis (0, -1, 0) left, (0, 1, 0) right, +0x30 clavicle, upper arm and forearm joints, +0x36 clavicle rotation, target and swivel tracks, +0x3C hand joint | rotation, vector, rotation |

Channel kinds per type come from 0xACA650 (1), 0xAC87C0 (2), 0xAC2610 (3), 0xACC2B0 (4), 0xAC7AF0 (5), 0xAB8280 (6), 0xABDB00 (7), 0xAB44E0 (8), 0xAC0BB0 (9), 0xAB98D0 (10), 0xAB81C0 (11) (confirmed). Types 5, 6, 9 and 10 exist in the code (joints per 0xABF270: 5 and 9 at +0x10, 6 at +0x30..+0x34, 10 at +0x28..+0x2E) but no P.T. rig uses them.

Joint table: u32 count (53), then (u32 unit, u32 StrCode32 of the bone name) per joint. Joints 0..52 are `SKL_000_WAIST` .. `SKL_216_RF53`, the first 53 bones of both och0 (90 bones) and plr0 (191 bones) in the same order (confirmed); the runtime maps joints to bones through a table at context +0x38 and recomputes the unit column from the unit joint lists (0xABF270, confirmed).

Masks: u32 unit count (18), u32 mask count (11), u32 offsets from the table start; each mask is u32 StrCode32 of the name, char[12] name, one float weight per unit: Lower (root, waist, spine, legs, feet, toes; chest 0), Upper (chest 0.5, neck, head, arms, hands, fingers), Head, LArm, RArm, LHand, RHand, MirrorL, MirrorR, CarryUpper, CarryLArm (confirmed from the data; layer weights for partial-body motion layers, likely; consumers not traced and not needed for single-layer playback).

## Rig evaluation

Pipeline for rig driven motions (addresses confirmed):

1. Sampling (0xAC2BD0 from 0xAA7720): each unit reads its tracks through a per-type sampler (0xAC8D50, 0xAC80F0, 0xAC1B60, 0xACBC50, 0xAC6B20, 0xAB8F70, 0xABCAA0, 0xAB5580, 0xABFB30, 0xABB240, 0xAB7A40 for types 1..11). The IK targets (vector channels of types 3, 8 and 10) are stored in motion space and moved into root space with the RIG_ROOT channels: `v' = R_root^T (v - p_root)` (confirmed). The waist position and all rotations are already root space: the Ocho waist has x = z = 0 on every frame and the horizontal motion is in RIG_ROOT; the player walk loops keep the waist near (0, 1.04, 0) while the foot targets travel 1.8 m with the root.
2. Parent-relative form for blending: 0xAC1E20 (leg) subtracts the waist position from the target and makes the swivel local to the waist rotation; 0xAB5AC0 (arm) subtracts the chest position from the target and stores the chest rotation and position in the unit data (confirmed). The evaluator adds them back, so for a single motion the round trip is exact.
3. Layers blend per unit (0xAC4440 and relatives; type 2 is a slerp with a polynomial acos, 0xAC83A0, confirmed; vectors linear, likely). 0xAC53A0 and 0xAC5510 add and subtract poses per type (additive layers).
4. Evaluation (RigToSkel job, profiler tag "Anim/RigToSkel" at 0xAE6860: 0xAE6710 creates it with vtable 0x1BD9660, whose method 0xAE66E0 calls 0xA8E9B0 -> 0xAA2BB0 -> 0xAC3E60): units in order; the results are model space rotations R and positions P per bone.
   - type 1 (0xAC8D40): nothing (root motion is used by the owner).
   - type 7 (0xABCA20): R = rotation channel, P = vector channel.
   - type 2 (0xAC7FB0): R = channel (model space, not relative to the parent); P = P_parent + R_parent * bind local.
   - types 4 (0xACBAA0) and 11 (0xAB7870): R = R_parent * channel; P as type 2.
   - type 3 (0xAC0F50): hip H = P_parent + R_parent * local(thigh); target T = P_parent + v; swivel q = R_parent * channel; pole a = q * (1, 0, 0). With d = T - H, D = |d|, L1 = |local(knee)|, L2 = |local(foot)|: e = normalize(d x (a x d)); x = max((D^2 + L1^2 - L2^2) / 2D, 0) when D < L1 + L2, else x = L1; h = sqrt(max(L1^2 - x^2, 0)); knee offset k = e h + (d / D) x; hinge b = e x (d / D). R_thigh = b u^T + k' t^T + (k' x b)(t x u)^T with u the unit axis, k' = k / |k|, t = local(knee) / L1; R_knee is built the same way from s = normalize(d - k) and local(foot) / L2. P_thigh = H, P_knee = H + R_thigh * local(knee). The foot is its own type 2 unit and lands on T when the target is reachable.
   - type 8 (0xAB4770): the clavicle as type 2 from channel 0; the chain from the upper arm (P_clavicle + R_clavicle * local(upper arm)) to T = P_chest + v, pole = channel 2 * (1, 0, 0), hinge = the unit axis, as type 3. Unit data +0x50..+0x57 (mode at +0x54, flags at +0x56) are cleared by the sampler 0xAB5580; flag bit 0 would call 0xAB3960, bit 1 with the context stretch flag scales the chain (not used by plain playback, confirmed).
   - the context stretch flag (+0x40) is anim object flag 0x800000, set only by 0xADCFF0 (an IK constraint job, vtable 0x1BD9290) and cleared after one evaluation; with it legs lengthen the thigh toward out-of-reach targets. Not used by the gimmicks (likely).
   - afterwards every bone the rig did not set takes its parent rotation and the position P_parent + R_parent * bind local (0xAA2BB0, confirmed); help bones are then driven by the .frdv (below).
5. Looping rig motions wrap to frame 0 at the end like the other gani units.

Ocho frame 0 (root space): waist (0, 1.276, 0), foot targets (0.125, 0.109, -0.063) and (-0.201, 0.180, 0.085), leg swivels about -90 degrees around Y (pole = forward, knees bend forward), hands at 1.12 to 1.14 m.

Verification: the Python model (`tools/motion.py`, `evaluate_rig`) was compared with the original functions 0xAC7FB0, 0xABCA20, 0xACBAA0, 0xAB7870, 0xAC0F50, 0xAB4770, 0xAC1E20 and 0xAB5AC0 executed natively (the eboot's code and data mapped at their link addresses in a Windows process and called through a System V thunk, the stack guard pointer redirected): every frame of the 17 rig driven motions and 3000 random poses with unreachable targets and random swivels agree within 0.17 mm and 1 - |q . q'| < 4e-8 (confirmed). `pt.exe --anim-test` compares the C++ rig (`src/engine/anim/rig.*`) with the `rig` section of the motion JSON (every 30th frame, all bones of och0 or plr0): max 1.4e-4 m, and the parent-relative round trip within 1e-5 m.

## Root motion of ShGimmick bodies

Frame order (confirmed). The game object update is a job graph (0x125F5C0); its middle stage 0x125F240 handles four categories, each with 0x1260A10 (the system's vtable+0x48 once), then 0x1260B60 for phases 0 and 1 (one job per instance, job vtable 0x1BD04A0, 0x96B850 calls vtable+0x50 with {instance, 0, phase}), then 0x1260DB0 (vtable+0x58). For ShGimmick (vtable 0x1BCE360) +0x48 is 0x954170, which runs every record's logic component (+0x80, vfunc +0x10; for Lisa OchoLogic_Update 0x12873F0), and +0x50 is 0x954270, which updates the record's body in phase 1. So the logic places the node first (body +0x90 = 0x1257130 -> 0x9646A0 writes the node matrix: LogicControl 1, Appear, Chase, Dash) and the body of the same frame then does:

1. +0x80 = 0x12570D0: entry+0x88 = dt x 299.7003 ticks. Gani frames are 5 ticks (`ticksPerFrame` of all 25 P.T. motions), so gimmick motions run at 59.94 frames per second (60000/1001).
2. +0x08 = 0x95E820: 0x9643E0 passes the ticks to 0xA8ECE0 (the layers advance; 0xAA7580 wraps at frames x ticksPerFrame), 0x964440 samples. The RIG_ROOT tracks step in 0xAD4A90 (kind 6: record+0x20 = new - previous with w = new y, record+0x30 = new) and in 0xAD6640 with 0xAD5E70 (kind 5: record+0x40 = new, record+0x50 = conj(previous) * new). On a loop wrap both carry the previous value across the seam (0xAD4A90: previous += first key - last key, confirmed; 0xAD5E70 turns the previous rotation by the rotation between the last and the first key, likely), so the delta of that frame is (end - previous) + (now - start).
3. 0xAA2760 (when the first layer is active and the base index is 0) reads the type 1 slots written by 0xAC8D50 (0 translation, 1 its delta, 2 rotation, 3 its delta) into the anim state: +0x20 = rotation delta, +0x00 = (slot 3 * conj(slot 2)) * translation delta = conj(previous rotation) * delta, +0x10 and +0x30 the current values.
4. +0x38 = 0x95E860: +0x40 (0x95EA50) serves entries with a character controller (flags & 7 == 5), +0x48 (0x95EAB0) the others through 0x964580 -> 0xAD6F20, which composes the anim state onto the node: world = world * [R(rotation delta) | translation delta] (absolute with anim flag 0x20). Gimmick bodies never get a controller: 0x953506 clears descriptor +0x40 bit 0, and 0x962D80 calls 0x963570 (controller 0x92839B779862, modules 0xAFC570, root adjust 0xAFAB90 through 0xAF6C10) only when it is set.

Consequences for Lisa (confirmed; RIG_ROOT figures from the motion data). No state is tested on this path: in every logic state the frame's RIG_ROOT delta lands on top of what the logic did.

| Ocho state | Logic | Root motion |
|---|---|---|
| 0 None | nothing | none: LogicControl 0 sends SetEnabled false (0x1253590 -> body +0x68 = 0x1257070 sets entry+0x9C bit 1) and every per-entry update skips the entry, motion included |
| 1 Warp | places her at the spawn on LogicControl 1 and Appear | Ocho: 1.613 m forward over the 1800-frame loop (still for 12 s, then steps of 0.2 to 0.5 m/s, largest 3.8 cm in a frame; 0.26 m of sideways sway, yaw -0.04 to 2.95 degrees, the loop ends on its start yaw). OchoStop (spawns 7 and 8): no net travel, sway of 0.10 m in x and z with a period near 106 frames, yaw -2.7 to 11.0 degrees, 0.84 degrees less at the end of the loop than at the start. The root keeps moving while she is hidden: Hide (0x12571B0) only sets node+0x1AC = -1, a draw mask |
| 2 Chase, 3 KillChase | places her 3 m behind the player every frame | OchoStop's delta of that frame on top (at most 3.2 mm and 1.36 degrees), never accumulated because the next frame places her again |
| 4 Dash | steps her toward the player from her current node position | OchoDash's RIG_ROOT is constant (zero translation, identity rotation, all 120 frames): nothing is added. While the layers still blend in from the previous motion the root slots come from the blend (not examined) |

Port (`GameObjects::UpdateOchoRootMotion`, `OchoLogic::ApplyRootMotion`, `GimmickAnimation::RigRootLoop`): after the logic and the pose update, the delta of the sampled RIG_ROOT between consecutive frames (restarted on a motion change, carried across the loop seam as above) is applied in every state and while hidden, only while the gimmick is enabled. The port takes the root of the new motion during a blend. Gimmick motions run on the body clock of step 1 (`GimmickAnimation`: frame = seconds x 299.7003 / ticksPerFrame, 59.94 per second; Ocho 30.03 s).

## .frdv (help bones)

File (confirmed): `FRDV`, u32 0x0BFFB0A8, u32 entry count, u32 0, u32 entry offsets, 0x80-byte entries. Demos with their own skinned models list them as `helpBoneFiles` (keyed by the model name) in the DemoStreamAnimation, and the parts' ModelDescription names one as `helpBoneFile` (och0, bab0, plr0). P.T. ships six: och0, plr0, dol0 and hsh0 with 21 entries each (types 2 x4, 7 x6, 11 x2, 12 x6, 13 x3, the same drivers on the same bone indices), bab0 (types 1, 2, 2: the heart) and coc0 (12 x type 2: the legs).

Entry (confirmed by the reads of 0xAE7420):

| offset | type | field |
| --- | --- | --- |
| 0x00 | u16 | type (1 to 0x16; types 0x10 to 0x16 call 0xAA3A60, 0xAF35B0 or 0xCD2A20 and are not used by P.T.) |
| 0x02 | i16 | driven bone |
| 0x04 | i16 | source bone |
| 0x06 | i16 | not read by the types P.T. uses |
| 0x08 | i16 | parent bone: base of the driven rotation and position |
| 0x0A | i16 | reference bone, -1 for none: the source rotation is taken as conj(R_ref) R_src |
| 0x10 | f32 | weight |
| 0x18, 0x1C | f32 | lower and upper limit (degrees) |
| 0x20 | u32 | axis index: types 11 and 13 turn about x, y or z (table 0x1BD9750: 0xAF3FC0, 0xAF40E0, 0xAF4200), type 1 slides that component |
| 0x24 | f32 | slide factor (types 12, 13) |
| 0x2C, 0x30 | f32 | slide limits (decimetres) |
| 0x34 | u32 | slid component of the bind offset (types 12, 13) |
| 0x40 | vec4 | axis a |
| 0x50 | vec4 | axis b |

Runtime (confirmed): the HelpBone anim plugin ("AnimPlugin/HelpBone", 0xAE6910, init 0xAE6F90, entry mask 0xAE7290; per frame 0xAE7410 -> 0xAE7420) works on the model space rotations and positions after the rig and 0xAA2BB0, entries in file order, each writing only its driven bone: R_d = R_p r and P_d = P_p + R_p o, where o is the driven bone's bind offset (the fmdl local position, the model's table at +0xF0, +0x10 per bone) with one component possibly replaced. With q = R_src or conj(R_ref) R_src:

- 1 (bab0's heart): r = identity; o[axis] = 0.1 limit(weight x 114.59155 x acos(|q.w|)), the angle of q in degrees.
- 2: r = slerp(identity, q, weight).
- 7: twist of q about a (q with the shortest arc from a to q a q* taken off; for opposite vectors a half turn about a x e, e the basis axis of a's smallest component, constants 0x143F110 and 0x143F120), r = slerp(identity, twist, weight); a negative weight gives the conjugate of the slerp with -weight.
- 12: as 7, and o[slide axis] = 0.1 limit(slide x (b . q a q*), slide min, slide max).
- 11: with v = q a q*, theta = 2 acos(sqrt((1 + a . v) / 2)), x = b . v, y = -(a x b) . v and g = sign(x) (2 / pi) atan2(|x|, |y| + 1e-10), r turns about the axis index by limit(weight x g x theta) with the limits in degrees (x 0.017453294).
- 13: as 11 with g = 1 - (2 / pi) atan2(|x|, |y| + 1e-10) for y >= 0 and (2 / pi) atan2(...) - 1 below, plus the slide of 12.

The slerp is the one the evaluator inlines: the shorter way round, linear weights from |dot| >= 0.999 (0x143F2E0), normalized; the limits apply as "below the lower one: lower; above the upper one: upper". At rest every entry reproduces its bind offset (SKL_502_LHMRS_HLP x = 0.1 x 0.03 = 3 mm, SKL_519_LMRF_HLP y = 0.1 x -1 = -0.1 m), so the slides are authored in decimetres. Only coc0 has help bones under help bones, and their entries come after their parents'.

Verification: the Python model (`tools/motion.py`, `evaluate_help_bones`) against 0xAE7420 executed natively (entry by entry, then whole files, on random model space poses from near identity to full turns; the angle table at 0x1BD9750 relocated by hand because the raw ELF holds it unrelocated): och0, plr0, dol0, hsh0, bab0 and coc0 agree on every bone within 5.8e-6 rad and 2e-7 m (360 poses). `pt.exe --anim-test` compares the C++ `anim::HelpBones` with `helpbones_<model>.json` (`tools/motion.py <frdv files> --json dump/demo --help-poses 30`): within 2.6e-6 rad and 3e-7 m.

Effect on Lisa's OchoDash, largest over the motion against the helper following its parent: SKL_513_RELBW_HLP turns 72.7 degrees, the buttock and knee helpers 30 to 47 degrees, the upper arm helpers SKL_502_LHMRS_HLP and SKL_511_RHMRS_HLP slide 19.6 and 18.5 cm along the arm, SKL_530_CLVR_HLP 8.1 cm. In the gc_p07_030 close-up with both arms raised the shoulders keep their volume and the tear at her left shoulder is gone.

Port: `src/engine/anim/help_bones.*` (`HelpBones::Parse`, `Evaluate` on rotations and positions, `Apply` on bone matrices). Gimmicks take the parts' `helpBoneFile` (Lisa, the Baby), demo models the DemoStreamAnimation `helpBoneFiles` or their parts' `helpBoneFile` (the ending's player); the help bones run after `ComputeBoneWorld` and before the skin matrices. `PT_HELP_BONES_OFF=1` turns them off for comparisons.

## .mog (player motion graph)

`ShPlayer_layers.mog` (4016 bytes, magic `FOXMOTIONGRAPH`) is the player's motion graph. Its tail holds 48-bit StrCode64 names (0xE60..0xF40: `Demo`, `init`, `Full`, `Null` and unresolved ones), the five node ids the locomotion code requests (0xF18..0xF38) and the 14 gani PathCode64s (0xF40..0xFB0). Records before it use self-relative offsets (confirmed for the name and clip references): 0x48-byte nodes with a name and a clip (STAND 0x7B1F790D0458 and `Demo` -> the 100-frame idle; 0x7CB04627A72F, 0x126ECBB6111A, 0x1FEA5DC25844, 0x458CF5FDAB8F -> the loops +Z, +X, -Z, -X; eight nodes named 0xB8A0BF169F98 -> the four 40-frame start clips (root 1.0 m) and the four 40-frame stop clips (root 0.2 m)), and 16 transition records of 0x28 bytes, all with interpolation 8. Transition endpoints and conditions are not decoded. 0x986420 (STAND) and 0x9874F0 (WALK) request the node ids from the table at 0x13CEBE0 (indices 0x22 and 0x25 + 2 x bucket, bucket from wrap(facing - stick heading) as in 0x989A10) through the motion control vfunc +0x130 with interpolation 8.0 (confirmed). The data tie the clips together: the last frame of the +Z start clip has exactly the foot targets of loop frame 36 (-Z start: loop frame 0), and every stop clip starts with the foot targets of its loop's frame 0 (confirmed from the data).

## Shared codec

The demo stream tracks and gani tracks are decoded by the same functions (0xAAB550 dispatch, 0xAD6980 quaternion, 0xAD46F0/0xAD4530 vectors, 0xAD5D70/0xAD5E70 rotations). Differences: gani data is inline (entry-relative offsets) and one block covers the whole animation; demo data comes per streamed block with per-track flags and the offset vector; gani units loop, demo units hold the last key.
