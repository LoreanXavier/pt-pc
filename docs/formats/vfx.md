# VFX (.vfx effect graphs, .vfxlf lens flares)

A Fox `.vfx` file is a compiled node graph: shapes (particles, lights, screen sprites) fed by emitter, life, vector and
material nodes, plus program effects (lens flare, sound). P.T. ships 87 unique effect files (111 copies across packages)
under `/Assets/sh/effect/vfx_data/` and lens flare descriptions under `/Assets/sh/effect/vfx_data/lensflare/*.vfxlf`.

`tools/vfx.py dump <files or folders> [--text] [--out dir] [--pathids pathid_list_ps4.bin]` prints a file as JSON or
as an indented graph, `summary` lists node classes and the files that use them, `schema` prints the class table.

Marks: confirmed (read in the game's code or shader code), likely (fits code and data), guess.

## 1. Files in P.T.

| folder | files | used by |
| --- | --- | --- |
| `dust` | `fx_sh_dstcomviw01`, `dstgls01_s1`, `dstgls01b_s1`, `dstptlviw01_s0`, `fx_tpp_dstbug01_s1` | window break glass, dust around the camera, ending |
| `filter` | `fx_sh_filfadaddviw01`, `filfadblnviw01` | ending screen sprites |
| `flare` | `flrbrm01_m1`, `flrcom01..03_m1`, `flrcom04_s5`, `flrlgt01_s3`, `flrlgt01red_s3`, `flrlgt02_s3`, `flrlgt03_s3`, `flrsct01_m1` | lamp flares and lights, ending street lights |
| `glass` | `glscom01a_s0`, `glscom01b_s0`, `glsrin01_s1` | wall lamp glass, ceiling lamp glass, rain on the window |
| `light` | `lgthal01_m1`, `lgthnd01_s5`, `lgtoch01_s3`, `lgtrdo01_s0` | red halo lights (mazes), hand light (ending), Lisa's face light (never created), radio light |
| `smoke` | `smkair01_m1`, `smkgnd02_m3`, `smkgnd03_s5`, `smkviw02_s5`, `fx_tpp_smkchrbrt01d_s0` | haze, ending |
| `sound` | 38 `fxsd_*` | sound only (one `FxSoundCallProgramEffectNode`), `audio.md` |
| `view` | `viwbld01_s0`, `viwdis01..03_s1`, `viwpic01..11_s1` | blood on the view, view distortion (the fake crash), picture ghosts in the mazes |
| `water` | `wtrbld01_s2`, `wtrbld01b_s2`, `wtrbld02_s1`, `wtrbld02b_s1`, `wtrbld03_s1` | blood pools and drips |

The `_sN` / `_mN` suffix is not used by the loader (guess: size class).

## 2. Binary layout (confirmed)

Little-endian, no padding, no offsets. The loader rejects a file whose parsed size differs from the file size.

| offset | size | field |
| --- | --- | --- |
| 0x00 | 3 | magic `vfx` |
| 0x03 | 2 | version (0 in all P.T. files) |
| 0x05 | 2 | node count |
| 0x07 | 2 | edge count |
| 0x09 | 6 | unused (zero) |
| 0x0F | | nodes, then edges |

Node: u64 class code (StrCode64 of the class name; the low 32 bits select the class), then every property of the class
in schema order, each as u8 element count followed by the elements. Element encodings:

| type | name | element |
| --- | --- | --- |
| 0 | bool | u8 |
| 1 | uint32 | u32 |
| 2 | float | f32 |
| 3 | Vector4 | 4 x f32 |
| 4 | String | u16 length, characters, NUL |
| 5 | StrCode64 | u64 |
| 6 | PathCode64 | u64 (file dependency, resolved through `pathid_list_ps4.bin`) |

Edge: u8 from node, u8 to node, u8 from type, u8 from port, u8 to type, u8 to port (6 bytes); with 255 or more nodes
the node indices are u16 (8 bytes). The type bytes are 2 for every P.T. edge. A node's inputs are the edges whose `to`
is the node, keyed by `to_port`.

## 3. Class schema

The property order is not stored in the file; it comes from the class descriptors registered in the game: for each
property the u32 name hash (the low 32 bits of StrCode64 of the name) and the type. The game registers 93 node
classes; P.T. files use 47. `vfx.py schema` prints the table.

Names: 227 distinct names cover 569 of the 839 property slots, every one verified by hash: names from the VfxTool
definitions (github.com/youarebritish/VfxTool, `Definitions/PT`) and 36 brute-forced names (`animationFrame`,
`blendMode`, `correctionType`, `flipU`, `flipV`, `keyframeMethod`, `lightAreaScale`, `lightAreaTranslation`, `lodType`,
`numFlare`, `shadowPenumbraAngleScale`, `shadowUmbraAngleScale`, `spreadRot`, `vectorName` and others). Several of
VfxTool's names are positional guesses that fail the hash check (`FxScrollAnimationMaterialNode` 0x972FD939 is
`cameraZOffset`, not `cameraFadeInFar`; `FxSpotLightShapeNode` 0x40ECFE36 is `hasSpecular`). Unnamed properties print as
`hXXXXXXXX`; section 9 lists the ones whose meaning is known.

## 4. Graph and time base

`FxModuleGraph` is the root: `allFrame` (effect length in frames), `playMode` (0 once: the effect stops emitting at
`allFrame`; 1 loop; 2 hold: the clock advances while it stays at or below `fadeOutStartFrame`, past it sets back to
`fadeInEndFrame`), `fadeInEndFrame`, `fadeOutStartFrame`, `updateType`, bounding box fields. Its inputs, in port order,
are shapes and program effects.

Frame values are 60 Hz frames (`lifeFrame`, `delayFrame` and similar fields are multiplied by 1/60 when compiled).

Shape inputs (ports of the `to` node):

| class | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| FxSpriteRotShapeNode, FxPlaneRotShapeNode, FxModelPrimitiveShapeNode | emit | life | material | position | scale | rotation | uv | color |
| FxSpriteShapeNode | emit | life | material | position | scale | uv | color | |
| FxSprite2DShapeNode | emit | life | position | size | uv | color | | |
| FxSpotLightShapeNode | emit | life | position | rotation | cone | color | | |
| FxPointLightShapeNode | emit | life | position | color | extra | range | | |

## 5. Emitters and lives

| node | semantics | confidence |
| --- | --- | --- |
| FxIntervalProbabilityEmitNode | per loop a window `[start, end]` in frames: `start = delayFrame + rand % (2 * delayFrameRandomRange)`, `end = start + lifeFrame + rand % (2 * lifeRandomRangeFrame) - lifeRandomRangeFrame`. Inside it, every `intervalFrame` frames: skip with probability from `probability` (percent, compared with `r * 99 / 2^32`), else emit `numMin + r % (numMax - numMin)` (`numMax` when equal). `fadeOutPosition` in [0, 1] of the window starts a linear fade of the count to 0 (`fadeOutReverse` fades in instead) | confirmed |
| FxDelayNumEmitNode | `num` particles once at `delayFrame` (+ random up to `delayFrameRandomRange`) when `lifeFrame` is 1, else `num` every frame during `lifeFrame` | confirmed |
| FxFirstLoopOnlyEmitNode | passes its input only in the first loop of a looping effect | confirmed |
| FxNumLodEmitNode | reduces its input count by `floor(n * percent) * t`, `t = min(1, d^2 / lodDistance^2)` of the camera distance (`inverse` flips `t`); percent is 0xFB5568AF | likely |
| FxConstLifeNode | `lifeFrame` | confirmed |
| FxRandomLifeNode | 0x9B076750 the base and 0x8F88FA93 a range (frames): uniform in [base - range, base + range] | confirmed |
| FxInfinityLifeNode | the particle never dies; the wrapped life is the period of its age for the time-based nodes | likely |

Randomness (confirmed): one xorshift generator (shifts 13, 7, 5) for every state. An instance is created with a seed
(the effect system's creation counter unless the creator passes one: demo effects pass their event's section start,
parts `effectRandomSeed` + the connection; a zero seed becomes 0xFFFFFF). The nodes of each emitter are numbered from
the seed, and a random node's state is `randomGatherSeedValue` with `randomGatherType` 2, `randomGatherSeedValue` + R
with 1, and node id + R with 0; the values are drawn once per particle at spawn. Every emitter also keeps a 16-bit
random value per particle for the UV animation's random start and flips. So a demo effect repeats the same particles
every time its event plays.

## 6. Vector nodes

Evaluated per particle from its age, life, life ratio, the effect time and the camera. `vectorType` 1 marks angles in
degrees.

| node | output |
| --- | --- |
| FxConstVectorNode | `vector * force` |
| FxColorVectorNode | `color` |
| FxRandomVectorNode | per particle uniform in `[randomMin, randomMax] * force`; `xySquere` uses one random for all components |
| FxCompositionVectorNode | `a + mix(b, maskValue, secondMask)` |
| FxMultiplyVectorNode | `a * mix(b, maskValue, secondMask)` |
| FxUniformVelocityVectorNode | `v * age` |
| FxUniformAccelVectorNode | `v * age + a * age^2` |
| FxUniformVelocityTimeVectorNode | `a * b` (velocity times a time node) |
| FxDragTimeVectorNode | time function for a velocity: `method` 0 life ratio, 1 age, 2 effect time; with `drag`/`scale` not 1: `scale * (1 - (1 - ratio)^drag)` (times life or effect length) |
| FxTimeScaleVectorNode | input scaled from `startScale` to `endScale` over the life, per mask component |
| FxKeyframeVectorNode | four piecewise linear curves (`xTimes`/`xValues`...), time from `timeRatio` (age, life ratio, effect time, effect ratio), combined with the input by `operatorMethod` (add, subtract, multiply, lookup) |
| FxOscillateVector2Node | `sin(2 pi a b)` |
| FxUVMapVectorNode | UV rectangle 0x4296121B with flips 0xAEADF7F1 (`flipU`), 0x902AA0EB (`flipV`) |
| FxUVMapRandomVectorNode | random cell of a `randomDivisionWidthGrid` x `randomDivisionHeightGrid` grid, random flips: per particle one xorshift of the node's state for the column (when the width is not 0) and one for the row; with `randomFlipU` or `randomFlipV` set, two more, bit 0 of each flipping U and V; a flip moves the rectangle's start to its far edge and negates its size |
| FxUVAnimeIntervalVectorNode | sprite sheet animation: grid 0x1D121378 x 0x9EC5A541, one cell every `animationFrame` frames, `clamp`; `randomStart` starts at the particle's 16-bit random value modulo the cells, `randomFlipU` and `randomFlipV` flip by its bits 0 and 1, each xor the fixed flips |
| FxCameraCorrectionVectorNode | input moved toward the camera by 0xBA7C713E metres (`correctionType` 1 from the particle, 0 from the effect origin) |
| FxCameraFollowVectorNode | position in camera space (offset 0x73D22AD2; 0x849D3E0C keeps only the yaw) |
| FxCenterScrollVectorNode, FxPoolVectorNode | wraps positions into a `range` box around the followed camera point (dust and smoke around the viewer) |
| FxCenterDistRateVectorNode | input scaled from `nearScale` to `farScale` by the distance to that point |
| FxCameraAngleVectorNode | angle between an axis of the effect (0x506F061C) and the camera's axis; its only user is the ending's hand light |
| FxInterpolateLineVectorNode | `beginPosition` to `endPosition` by particle index / count |
| FxSpreadVectorNode | a latitude uniform in [pi/2 (1 - `elevation` / 180), pi/2] (180: the upper hemisphere, 360: the sphere), an angle around y uniform in [0, `rangeAngle`] and a length, drawn in that order; with 0xE6B68466 the angle from +y is fixed at `elevation` / 180 x 1.57 (180: a flat disc); with 0xD8F07EBD as well, one length for the batch and evenly spaced angles. The length is `force`, or with `forceType` != 0 uniform in `force` -/+ 0xEF65426D. Rotated by the quaternion 0x05B462F3 |
| FxReceiveVectorNode | named parameter set by the owner (demo parameter events, section 8.2), else `defaultVector`, times `force` |
| WindFxVectorNode | `airResistanceRate` x the global wind rotated into the particle's space |

Confidence: likely for all rows (formulas fitted to the data and checked on screen); the constant, random, UV, spread
and receive nodes are confirmed.

## 7. Shapes and materials

- FxSpriteShapeNode, FxSpriteRotShapeNode: camera-facing quads (rotation roll from the rotation input), `centerU`/`centerV`
  pivot, `baseSizeScale`. FxPlaneRotShapeNode: quads in the effect's space, turned by the rotation input X first, then Y,
  then Z (`Rx * Ry * Rz` in row vector form). `axisFix` then turns the plane about one effect axis toward the camera: 1
  about X, 2 about Y, 3 about Z (the hand light's beam planes use 3; the street lamp lens planes and two blood effects
  use 2). 0x94390DB1 marks shapes whose particles are drawn and sorted one by one (likely); `sortMode`/`sortOffset`
  sort per draw. `localSpace` false keeps particles at their spawn transform.
- FxModelPrimitiveShapeNode (confirmed): `modelFile` is the StrCode64 of the model's path. Every particle is a draw of
  LOD 0 of all meshes of the model (positions, texcoord 0, normal, tangent; the model's materials and vertex colours are
  not used), drawn with the node's material. Flags: bit 0 `enable`, bit 1 0x0E49C84B (sort the triangles by depth),
  bit 2 `cullFace`, bit 3 `invertFace`, bit 4 0x7241DEDA (texcoord `uv.xy + uv.zw * t` from the uv input). The world
  matrix is `S * Rx * Ry * Rz * M` in row vector form. The colour input is written to every vertex as RGBA8, each
  channel the low 8 bits of `int(c * 255)`, so a channel above 1 wraps. Used by the ending's street lamp lenses
  (`flrcom01_m1`, `flrcom02_m1`) and the bathroom bulb (`flrbrm01_m1`).
- FxSprite2DShapeNode (confirmed): screen sprites in a 0x32FF2253 x 0x91365199 pixel space (1280 x 720), origin top
  left, y down; the quad's top left is `origin + position - (centerU, centerV) * size`; `origin` is (0, 0), or the
  projected effect position when 0x55389195 is set; 0xDE02160D is the blend mode (1 alpha, 2 add). Drawn after the
  post chain on the final image.
- FxSpotLightShapeNode, FxPointLightShapeNode: lights. Spot direction is local -Y after the rotation input. cone input =
  umbra, penumbra; color.w = lumen; `innerRange`, `outerRange`, `attenuationExponent`, `castShadow`, `hasSpecular`,
  `viewBias`, `shadowBias` (x 0.001), and 0xA07B1E26, a projected mask texture (set only by the hand light). Point extra
  = temperature, -, lumen scale. `shadowUmbraAngleScale` and `shadowPenumbraAngleScale` scale the cone angles for the
  shadow map, whose field of view is the scaled umbra. Light area: `lightAreaTranslation`, rotation 0x733D3780 (a
  quaternion), `lightAreaScale` and `enableLightArea` (0xFFE7D17F) clip the light to a box like a level light; the
  ceiling lamp flare lights, the maze halo lights, the radio light and one spot of the bathroom bulb set one.

Materials:

| node | P.T. shader | meaning |
| --- | --- | --- |
| FxLightInfluenceMaterialNode | `*_LitDP` | color x (`ambientRate` x sky + `directionalLightRate` x directional + `pointLightRate` x the sum over three lights of colour x max(0, 1/d^2 - d^2/R^4)); `softBlend` with the fade distance `softBlendFactor`; `opaque`; camera fade `cameraFadeInNear`/`Far`; `cameraZOffset` |
| FxDynamicLuminanceMaterialNode | `*_DL` | `blendType` (0 alpha, 1 add, 2 subtract, 3 multiply, 4 min, 5 opaque), `shaderType` 0 `FastDraw_DL`, 1 `Softblend_DL`, 2 `TexAnmSoftBlend_DL`; luminance factor interpolated from 0xAB96FA51 at `minExposure` to 0xFC08FF2F at `maxExposure` by the exposure |
| TppLiquidMaterial2Node, TppLiquidMaterial2HNMNode | `Primitive_Liquid2Final`, `_hnm`, `LiqSprt2Final` (sprite shapes) | see below |
| FxScrollAnimationMaterialNode | `Prim_Poly_RainScroll` | two screen-space rain layers with animated rotation, `luminance`, soft depth fade |

Liquid (confirmed from the shader): the texture is a normal map (RGB, or for HNM x in alpha, y in green, z derived);
the normal is rotated into the quad's frame (planes: the quad's own axes; sprites: screen right, up and toward the
camera, turned by the texture rotation). Material constants: `m[0]` = (`transparency` T, `roughness`, 0, refraction
0xF6B16443 in pixels), `m[2]` = (`ambientRate`, `pointLightRate`, `directionalLightRate`), `m[3]` = (Fresnel base
0xBD8530BE, Fresnel power 0x3B831F9F, reflection scale 0x62133294). Textures: `textureFile`, the scene colour, the
reflection cube 0x23E90E5F (default `gr_cub01_ks_cbm_nmp`). Result:

```text
color = inColor.rgb * ((1-T) * sat((1-T) * dot(N, V) + T) * scene + hemisphere ambient * ambientRate + (1-T) * diffuse lights)
      + T^2 * scene + specular (exponent 257 at roughness 0)
      + reflection scale * (F0 + (1 - F0) * (1 - dot(-V, N))^power) * cube(reflect(V, N))^2
scene = scene color at the pixel offset by N.xy * refraction
```

`opaque` enables the alpha test at 0.5. Glass (T = 1) shows the refracted scene plus highlights; the blood pools
(T = 0) are lit red surfaces. The refraction samples a copy of the scene buffer taken before the liquid draws; the
offset is in pixels of the target the draw renders to (full resolution for opaque liquids, half resolution for the
others), folded back at the edges. The view distortion effects are such sprites: `fx_sh_viwdis01_s1` (refraction 500,
a sprite 0.2 m in front of the camera) moves blocks of the image by up to 500 half-resolution pixels; `fx_sh_viwdis02_s1`
stretches the texture's last column so horizontal bands shift left, right, up or down for the 30 s of the number chant,
the tearing of the fake crash screen.

Soft particles (confirmed): `softBlendFactor` is the fade distance in metres against the scene depth (the radio glow
0.2 m, the blood drops 0.015 m, the ending's ground smoke 10 m). Fog: the `Prim_*` pixel shaders read the fog volume at
the particle's view depth; with the alpha blend the colour becomes `colour T + inscatter`, with every other blend the
alpha is multiplied by the transmittance T.

Wind: the global wind object's velocity, with a turbulence term from `speedTurbulentRate`, `rotTurbulentRate` and
their cycles, reaches WindFxVectorNode times `airResistanceRate`. P.T. has one WindGlobal, in the ending: velocity
(0, 0, 5), no turbulence; the only wind node is the ending dust `fx_sh_dstcomviw01` (`airResistanceRate` 0.1).

Program effects: `TppLensFlareProgramEffectNode` registers one light per effect instance with the lens flare manager:
`lensFlareName`, `lux`, `baseDistance`, `limitDistance`, temperature 0x2642E962, `offsets` (the light position in the
effect's space), `drawPriority`, `numFlare` (1 in P.T.), cone angle 0x430D5D92 (degrees; 360 except the hand light, 90),
0x32CA14EC (distance scaling uses the shape's `limitDistance`) and 0xF856246F (depth tolerance of the occlusion taps in
metres). `FxSoundCallProgramEffectNode`: `soundEvent` and `soundStop` as Wwise ids, 0xE3A9CADA the stop curve, 0xD2ECAC68
the stop fade in seconds, `lodDistance`, and a flags word (bit 0 `soundEvent` set, bit 1 `soundStop` set, bit 2 stop the
playing id over the fade when the instance goes, bit 6 `enableLod`). Besides the 38 `fxsd_*` files, four effects carry
it: `fx_sh_viwdis01_s1` (`Play_sfx_bug_loop_01`), `fx_sh_viwbld01_s0` (`Play_sfx_splinkle_blood_01`), `fx_sh_wtrbld01b_s2`
(`Play_sfx_water_drop_b`) and `fx_sh_wtrbld02_s1` (`Play_sfx_refri_blood_01`).

## 8. Where effects are created

### 8.1 Levels

`FxLocatorData` entities (`vfxFile`, transform) in the stage files: pt14_start 2, pt14_hallway 58 (40 visual, 18 sound),
maze A 59, maze B 71, maze C 4, ending 34. An effect runs while its entity's body is visible and enabled (trap scripts
toggle them, e.g. `enable_vfx_dust_glass`) and its stage is active; the instance is created suspended when the block
loads and started at the block's activation, when `createOnInitialize` is set (false only for the two puzzle blood
effects `fx_sh_wtrbld03_s1`).

### 8.2 Demos

| functor | role |
| --- | --- |
| FxEffectCreateEventFunctor and five others | create an instance: path, `instanceName`, position, rotation; at the event's section end its emitters stop and its particles live out their lives |
| 0x62B47A5B81EC | kill every live instance named `instanceName` |
| 30 parameter functors (`color`, `Range`, `ParticlesSize`, `Spotlight_Cone_Angle`, `SpotlightColor`, `SpotlightBeamColor`, ...) | set a named receive parameter (vector4, optionally interpolated) |
| EffectConnectToNullFunctor | attach to an `Eff_Null_*` locator |

The ending sets an instance's parameters on the frame it is created.

### 8.3 Gimmick parts

Parts files (`/Assets/sh/parts/chara/*/*.parts`) hold `EffectDescription` entities: `partName`,
`connectDestinationSkelNames` / `CnpNames`, `offsetSkelPositions` / `CnpPositions`, `generalSkelParameters` /
`CnpParameters`, `effectConnect`, `visibleModelWithEffect`, `createStartEffect`, `effectRandomSeed`, `effectKind`,
`effectFileFromVfxFileLoader`, `effectFileFromFilePtr`. The gimmick records switch them by index:

| record | index 0 | 1 | 2 | retrigger |
| --- | --- | --- | --- | --- |
| CeilLamp | `CeilLampGlass` (glscom01b) | `CeilLampFlareLight` (flrlgt01: spot light and lens flare) | `CeilLampFlareLightRed` (flrlgt01red) | no |
| Freezer | `FreezerBloodTop` (wtrbld02 at CNP_FRZ_OPEN) | `FreezerBloodEye` (viwbld01) | | yes |

Connection matrix: bone world matrix times a translation by the offset in bone space; `effectKind` 1 then adds
`general.xyz` in world space, 2 rotates by `general` as a quaternion. The random seed is `effectRandomSeed` + connection
index when non-zero. `Face_Light` (Lisa, lgtoch01) is registered by nothing, so it is never shown.

## 9. Unnamed properties with a known meaning

| hash | class | role | confidence |
| --- | --- | --- | --- |
| 0x94390DB1 | shapes | draw and sort particles one by one | likely |
| 0x32FF2253, 0x91365199 | Sprite2D | screen width, height | confirmed |
| 0xDE02160D | Sprite2D | blend mode (`blendMode`) | confirmed |
| 0x55389195 | Sprite2D | origin at the projected effect position | confirmed |
| 0x4296121B | UVMap | UV rectangle | likely |
| 0x1D121378, 0x9EC5A541 | UV nodes | grid width, height | likely |
| 0xBA7C713E | CameraCorrection | offset toward the camera | likely |
| 0x73D22AD2, 0x849D3E0C | CameraFollow | offset, yaw only | likely |
| 0xAB96FA51, 0xFC08FF2F | DynamicLuminance | luminance at min and max exposure | likely |
| 0x23E90E5F, 0xF6B16443, 0xBD8530BE, 0x3B831F9F, 0x62133294 | Liquid | reflection cube, refraction, Fresnel base, power, reflection scale | confirmed |
| 0x9B076750, 0x8F88FA93 | RandomLife | base life, range (base +- range) | confirmed |
| 0xEF65426D, 0x05B462F3, 0xE6B68466, 0xD8F07EBD | Spread | length random range, rotation, fixed polar angle, even angles per batch | confirmed |
| 0x506F061C | CameraAngle | axis | confirmed |
| 0xA07B1E26 | SpotLight | mask texture | confirmed |
| 0x733D3780 | SpotLight, PointLight | light area rotation (quaternion) | confirmed |
| 0x2642E962, 0x430D5D92 | lens flare node | temperature, cone angle | confirmed |
| 0xF856246F, 0x32CA14EC | lens flare node | occlusion depth tolerance, use the shape's limit distance | confirmed |
| 0xFA4CEAA3 | Receive | global parameter | guess |
| 0x1364CE7E, 0x6B880E78 | ScrollAnimation | initial rain angles in degrees | confirmed |
| 0xACB0CDBB, 0xF1A308EB | ScrollAnimation | rain angle swings in degrees | confirmed |
| 0xFB5568AF | NumLod | percent | likely |
| 0x7241DEDA | ModelPrimitive | texcoord from the uv input | confirmed |

## 10. Lens flare files (.vfxlf)

Fox2 data sets (`fox2.md`) loaded by name from `/Assets/sh/effect/vfx_data/lensflare/<lensFlareName>.vfxlf`:
`fx_sh_lfrlgt02` (hallway sconce), `lfrlgt03` (corner and lobby ceiling lamps, maze C), `lfrlgt01` and `lfrlgt01red`
(ceiling lamp), `lfrgun00` (hand light). The root `TppHandLightLensFlareRoot` holds `exposureBlend`, `needCollisionCheck`
and the `shapes` list; every file also carries unlisted template shapes (Halo1, ArrayGost1, HSmear1), which are never
drawn. A root or listed shape whose entity `flags` bit 0 is clear is not drawn either. No listed shape is a
`TppLensFlareShapeArray` or `TppLensFlareShapeCircle` and no material sets `arcAlphaField` or `maskShape`, so of the
flare shaders P.T. uses only `Draw2D_TppLensFlare`. Everything below is confirmed.

Per light and frame:

- `d` = distance from the camera; cone factor `c` = (dot(direction to the camera, Z) - cos(a/2)) / (1 - cos(a/2)), 0
  below, with Z the third row of the effect instance's matrix and `a` the node's cone angle.
- Distance fade ((d - limit) / (base - limit))^2 clamped to [0, 1] and ratio base / d from the node's `baseDistance`
  and `limitDistance`.
- Visibility state: shielded when the light is behind the camera; every 6 frames a light in front becomes visible.
  With `needCollisionCheck` (only `lfrgun00`) a camera-to-light raycast decides instead. Fade = 1 - min(1, shielded
  time / `shieldFadeOutTime`); while visible it moves to 1 over `shieldFadeInTime`.

Per shape:

- Drawn while visible or still fading out, and only with `c` > 0.
- Position `p` (NDC, y up): `offsetType` 0 `baseOffset`, 1 light + `baseOffset`, 2 light x `offsetScale`, 3 and 4 scale
  only x or only y.
- Fields at `p`, or at the light with `scaleFieldPickSunPositionFlag` / `alphaFieldPickSunPositionFlag`: distance
  max(|x|, |y|) for `shapeType` 0, else sqrt(x^2 + (0.5625 y)^2); `innerValue` below `innerScale`, then interpolated to
  `centerValue` at `centerScale` and `outerValue` at `outerScale` (`interpType` 0 linear, 1 cosine, 2 t^2, 3 1 - (1 - t)^2;
  all P.T. fields use 0).
- Rate graphs (11 samples over [0, 1], clamped) at `c`: `angleScaleGraphX/Y` scale the size, `angleAlphaGraph` the alpha.
- alpha = alpha field x fade x angle graph x root colour red; size = scale fields x angle graphs; then `distanceScaling`
  1: size x base / d; 2: alpha x distance fade; 3: size x base / d x saturate((limit - d) / (limit - base)).
- Rotation: `rotateType` 1 -atan2(p.y, p.x), 2 +atan2, 3 screen-space rotation fields (unused in P.T.), plus
  `baseRotate`.

Quad: screen space [0, 1] with y down; corners at rotation + (-135, -45, 135, 45) degrees with UV (0, 0), (1, 0),
(0, 1), (1, 1), offset (A cos, B sin) from the centre with A = 0.25 x `width` x size x and B = 0.25 x `height` x
size y / 0.5625 (so equal pixels). Root colour: with `exposureBlend` b > 0 (only `lfrgun00`) c = exposure x 10.267 x the
temperature colour of the node's temperature and `lux`, normalized when longer than 1; colour = mix(white, c, b), of
which only red is used.

Draw: a 2D layer after the post chain, additive, one batch per material; texture 0 the shape texture, texture 1 the
scene depth. The pixel shader takes three depth taps around the light pixel, v = sum of saturate((tolerance + scene
depth - light w) / tolerance); rgb = texture x colour, alpha = texture alpha x vertex alpha x (1 - (1 - v/3)^2).

In the first corridor the sconce (`lfrlgt02`: `Flare1` width 1 and alpha 0.6, `Flare2` width 0.25 and alpha 0.85, mode 3,
limit 20 m, tolerance 0.3 m) gives a core on the lamp and a soft halo of 0.4 screen widths radius at 3 m.
