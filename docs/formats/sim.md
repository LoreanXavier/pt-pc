# Bone simulation (`.sim`)

A `.sim` is a fox2 DataSet with one `SimOnPhysics` and its `PhObjectDesc`: the rigid bodies, shapes and joints that move a model's
soft bones. A model gets one through its parts: a `SimDescription` (`simFile`, `isActive`) in the `.parts` beside the `ModelDescription`.
Three files ship; two are used.

| file | in | named by | bones |
| --- | --- | --- | --- |
| `/Assets/sh/chara/plr/Fox_Files/plr0_main0_def_s00.sim` | ending.fpkd, plparts_normal.fpkd | plr0_main0_def_v00.parts | the player's jacket, hood, drawstrings, hair, trouser hems (85 bodies, 78 simulated) |
| `/Assets/sh/chara/bab/Fox_Files/bab0_main0_def_s00.sim` | resident.fpkd, pt14_hallway.fpkd | bab0_main0_def.parts | the baby: the back and the front of the soft skull (`SKL_602_HEADB_SIM`, `SKL_603_HEADF_SIM`, on `SKL_005_HEAD`) and the belly (`SKL_600_WAIST_SIM`, on `SKL_000_ROOT`) |
| `/Assets/sh/chara/pab/Fox_Files/pab0_main0_def_s00.sim` | resident.fpkd | nothing | the paper bag's two `SKL_60x_PAB_SIM` bones; pab0_main0_def.parts has no `SimDescription`, so the original never simulates the bag |

Data layout: `sim_physics.h` (`SimRig`). The baby's units: two keyframed bodies (motionType 1, spheres) on ROOT and HEAD, three
dynamic capsules (motionType 2, radius 0.01, half length 0.03, mass 0.1, damping 0.01) 4 cm below their bones, each hanging from its
parent on a ball socket at the bone with a 35 degree cone around refB (the waist's is 43.2 degrees from the bind direction refA, the
back of the head's 18, the front's 14.4) and the twist locked. The skull's sim bones carry up to 55 % of the head's top and back
vertices (930 and 1219 vertices), the waist's 640 of the belly.

## The original's solver

Not Havok: the engine's own rigid body world, one per simulated model, set up by the SimEngineOnPhysics object (vtable 0x1C05CC0).

- World (FUN_00fc8f30 over the defaults of FUN_00f1cf80): gravity (0, -9.8, 0), sub steps of 1/60 s, 3 velocity and 2 position
  iterations. A frame's time is cut into 1/60 s steps, the last step taking what is left when that is at most 1.25 steps
  (FUN_00f22d10); the frame time is held to 1/15 s (DAT_01b8cd34, FUN_00f0ca50).
- Bodies (FUN_00f19670, FUN_00f2b530): velocities multiplied by `pow(1 - damp, dt)` (precomputed for 1/60 and 1/30), speeds held under
  `maxLinearVelocity` m/s and `maxAngularVelocity`, which the file gives in degrees a second (180: pi rad/s).
- Keyframed bodies: the association (PhxAssociation, FUN_00f8d8c0 -> FUN_00f09990 -> FUN_00f2a700) gives each the pose of its bone as
  a target once a frame; every sub step (FUN_00f236e0 -> FUN_00f2a7e0 with the step's 1/h) sets the velocity that reaches it at the end of
  that step, so in a frame of several steps the body moves in the first and rests in the others.
- Ball socket with a cone and a locked twist (0xF37810 -> 0xF4D4E0, refA and refB turned by the object's rotation into the bodies'
  frames): the joint's rows use 0.5 and 0.5 (FUN_00f49a00), the cone and twist rows 0.05 and 0.95 (set after 0xF4D4E0). Read as the
  share of the error taken into the velocities and the share moved out in the position pass, they match the original best (below).
- The twist the joint holds at zero is measured from the shortest turn that takes the child's bind axis onto refB and then onto the
  child's current axis (the bind rotation carried along those two turns); measured that way the PS4's twist averages 0.05 to 0.34 degrees.
- The SimEngineOnPhysics update (0xFC86A0) also moves the whole world with the model when the model jumps further than
  `SimDaemon.inertialMax` a second; the baby never moves.
- A frame draws the simulated bones as the bodies stood after the step before, whose targets were the bones of the frame before: the
  soft parts trail the animation by one frame. Measured against the parent of the frame before, the PS4's three bones sit on their 35
  degree cones in every frame (34.99, 35.00 and 35.02 degrees, spread 0.03 to 0.07); against the parent of their own frame the cone
  angle spreads by 0.9 to 1.4 degrees. That lag is the extra movement of the PS4's baby head: in the replay below the back of the skull
  moves 1.50 degrees about its mean on the PS4, 0.34 in the port without the lag, 1.58 with it.

## The port

`anim::SimRigid` (`sim_rigid.h`): the same world, bodies, sub steps, damping, speed limits and hard keyframes; the joint solved as a
3x3 block, the cone and the twist as single rows, then the position pass. `GimmickAnimation` loads the parts' `SimDescription` and steps
the simulation after the help bones while the gimmick is enabled, active and shown, restarting from the pose when it comes back
(`PT_SIM=0` turns it off, `PT_SIM_TRACE=1` logs each simulated bone's turn from its animated pose every frame). The bones are drawn one
frame of the original (1/29.97 s) behind the animation at any frame rate, the bodies interpolated between steps (`SimRigid::SetDelay`;
`PT_SIM_DELAY=<seconds>` sets another delay for comparisons, 0 draws the bodies as the step left them). The player's model runs on
it too, in the demos (`DemoSystem::SimulateModel`) and in play (`PlayerBody`: third person, photo mode, the mirror, Survive the
Night; while running the cloth follows the pose, as before). Until 9 November 2026 the player's model ran on position based dynamics
(`anim::SimPhysics`, removed).

What the player's rig needs beyond the baby's:

- Contacts: a dynamic body touches the keyframed bodies of another `collisionId`, the capsules on SKL_000_WAIST and SKL_002_CHEST
  (id 0; every other body has id 1, so the hems pass through the other leg's body, as the PS4's do by 4 to 8 cm). A capsule (shape
  type 5) lies along its local y, radius size.x, half length size.y; other shapes are spheres of radius size.x. A contact pushes only
  what sinks in beyond `permittedDepth` (0.01), with the limits' 0.05 and 0.95; the hit bodies' friction is 0. Without contacts the
  jacket's front strips sink 10 to 11 cm into the waist's capsule, the PS4's 2.7 at most.
- The world goes with the model: SimEngineOnPhysicsParam.convertMoveToWind (set for the player) makes the SimEngineOnPhysics update
  (0xFC86A0) move every body by the model's displacement since the last frame and pass the opposite velocity to the wind control as
  air. Without it the update moves the world only by what goes beyond SimDaemon.inertialMax a second (not ported: no model without it
  moves). The drawn delay keeps the bodies in the model's frame, as the original writes them to the bones.
- Wind: the SimWindControl's bones (the hood's five wind bodies) take the air's speed relative to the body, times 2 and the file's
  coefficient, a second (the control's force was not traced; from 0 to 8 the replay's wind bodies change by under one degree).
- Not ported: sleep (as for the baby) and SimDirectionControl (no file uses it).

## Measured against the original (8 November 2026)

Capture `baby_sim_f060` (tools/shadps4/routes/baby_sim_f060.route): the f060 sink in the emulator, every skinning constant of the baby's
G-buffer draw (vertex shader 87c497ccf0b89de6 in this emulator, `VSBones` = bone matrix x inverse bind) traced in 61 consecutive frames
(3600 to 3660) and every 15 frames to 4620. The original renders the baby at LOD 2 (18,648 indices) there; the port draws LOD 0.

Turn of each simulated bone from its parent (degrees):

| | PS4 mean (min, max), 125 frames | port mean, 2,851 frames |
| --- | --- | --- |
| belly `SKL_600_WAIST_SIM` | 12.15 (5.85, 25.38) | 11.84 |
| back of the skull `SKL_602_HEADB_SIM` | 52.14 (45.99, 56.90) | 52.10 |
| front of the skull `SKL_603_HEADF_SIM` | 22.91 (16.82, 33.45) | 23.14 |

Replay (`tests/sim_rigid_test.cpp`, fixture `tests/fixtures/baby_sim_ps4.txt`): drawn one step behind like the original, started
from where the original drew the bodies at 3601 and stepped with the original's frame times and animated bones, over 59 frames:

| | RMS error (worst) | turn from the parent, mean and spread: PS4 | port |
| --- | --- | --- | --- |
| belly | 0.51 (1.19) | 11.88, 2.91 | 11.84, 2.91 |
| back of the skull | 0.19 (0.64) | 52.09, 1.50 | 52.19, 1.58 |
| front of the skull | 0.29 (0.63) | 22.44, 1.75 | 22.54, 1.76 |

Drawn without the lag the errors were 0.92, 1.47 and 1.42 degrees and the spreads 2.44, 0.34 and 1.12: the right sag, too little
movement. Read as other things (velocity-only Baumgarte limits, damped velocity rows without a position pass, soft limits) the same
constants gave 2 to 35 degrees.

The emulator ran these frames at 57 to 107 ms, so its lag is longer than a PS4's steady 1/30 s and its sub steps stop at the 1/15 s
cap; the port's delay is the PS4's own frame.

The draw's other bones in the same 125 frames: the 20 animated bones below the root match a frame of `Baby3` to 0.06 degrees on
average (0.08 at most; no other layer moves them), and the three heart help bones of `bab0_main0_def.frdv` (a SlideByAngle and two
Follow entries on `SKL_002_HEART_DRIVE`, which the draw does not carry), evaluated as the port does from that frame on the PS4's chest,
match the PS4's to 0.02 degrees on average (0.38 at most) and 0.15 mm.

Replay of the player (`tests/sim_jacket_test.cpp`, fixture `tests/fixtures/jacket_sim_ps4.txt`, capture
tools/shadps4/routes/jacket_sim_ending.route: the ending street, frames 4180 to 4300, the skinning constants of every draw of plr0,
vertex shader 30c4c697248d0bb2 in this emulator): 76 simulated bones, RMS 7.86 degrees, the jacket's strips 6.2 (position based
dynamics: 10.2). The emulator ran these frames at 0.1 to 0.7 s with a camera cut among them, so the bodies take jumps no PS4 frame
has; the hems, swung by the legs in steps of a quarter second, remain the largest part (31 degrees RMS; the PS4's hang 48 on average,
the port's 35), the strips follow the PS4's direction to a few hundredths and hang 7.9 degrees from their parents against 10.1.

Not ported: the bodies' sleep settings (`sleepEnable` with 0.1 m/s, 30 degrees a second and 0.25 s; FUN_00f19670 does not read them).
In the replay the head turns slower than 30 degrees a second in 48 of 60 frames and the PS4's bones still sit on their cones against
the head of the frame before in every frame, so no body sleeps there.
