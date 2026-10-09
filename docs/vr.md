# VR mode (experimental)

A PC extra: P.T. in a VR headset through OpenXR. **It has not been tested on a real headset.** Everything below was built and
checked against a headless test runtime (see "Testing without a headset"); a real runtime and headset may show problems those
tests cannot. Off by default; with it off the game is unchanged (the render check gives 0.00 against the build before it, and
the walkthroughs pass).

## Turning it on

- PC settings > Extras > VR (experimental) > VR mode: On, then restart. Or `pt.ini` `[vr] enabled = 1`, or `pt.exe --vr` for one
  run (`--no-vr` never starts it).
- It needs an OpenXR runtime with `XR_KHR_vulkan_enable2` (SteamVR, the Meta Quest Link / Oculus PC runtime, Windows Mixed
  Reality, Varjo, Pimax, Monado) set as the system's active runtime, and a connected headset. Without them the game logs why
  and starts as without VR: `vr: off for this run: xrCreateInstance (is an OpenXR runtime installed and active?):
  XR_ERROR_RUNTIME_UNAVAILABLE (no OpenXR runtime is installed or active)` in `pt.log` with no runtime,
  `xrGetSystem (is the headset connected?): XR_ERROR_FORM_FACTOR_UNAVAILABLE` with a runtime but no headset. The VR setting
  stays on, so the next start tries again; turn it off in Extras or with `--no-vr` to stop the attempt.
- Where to get a runtime: SteamVR (Steam; it registers itself as the active OpenXR runtime from its settings, Developer >
  "Set SteamVR as OpenXR runtime"), the Meta Quest Link app for a Quest over Link or Air Link, or the Mixed Reality
  OpenXR runtime for a WMR headset. Only one runtime is active at a time; the port uses whichever the system names.
- The Khronos OpenXR loader `openxr_loader.dll` 1.1.63 ships next to `pt.exe` (KhronosGroup/OpenXR-SDK-Source release, the
  OpenXR.Loader package; the loader is Apache-2.0, the headers Apache-2.0 OR MIT; both texts in `licenses/`). The build
  downloads the package and the two license texts and checks them by SHA-256 (`cmake/OpenXR.cmake`, like the upscaler SDKs;
  `-DPT_OPENXR=OFF` builds without VR). The game loads the DLL with `LoadLibrary` only when VR is on, so without VR it is never
  touched.

`pt.ini` `[vr]`: `enabled`, `flashlight` (0 the head, 1 a controller), `flashlight_hand` (0 left, 1 right), `turn` (0 snap,
1 smooth), `snap_degrees` (30), `smooth_speed` (90 degrees a second), `resolution_scale` (1.0: the eye images at the size the
runtime recommends, 0.5 to 2), `height_offset` (0.0 m by default, adjustable from -0.5 to +0.5 m), and `world_scale` (1.0 by
default, 0.5 to 2.0). The height offset moves the eye anchor, HUD, virtual screen and controller flashlight vertically; the head
collision ray starts from the adjusted anchor too. World scale multiplies tracked head and controller translation relative to
the recentered head, while orientations, eye separation, and HUD and virtual screen sizes stay unchanged. A zero offset and a
world scale of 1 preserve the original mapping. These settings and the flashlight and turning options change on the Extras > VR
page while playing.

## How it works

### Vulkan and the session (`src/engine/xr/xr_host.cpp`)

`XR_KHR_vulkan_enable2`: the runtime creates the Vulkan instance and device (`vk::ContextCreator`, so it can add what it needs)
and names the GPU the headset is on. The session gets four swapchains: one per eye at the recommended size times
`resolution_scale`, a 1920x1080 HUD and a 1920x1080 virtual screen, preferring 8-bit sRGB formats (the port's images hold
sRGB-encoded values; `shaders/xr_copy.frag` writes linear values and an sRGB swapchain encodes them on write, exactly). Spaces:
LOCAL for everything, VIEW for the head. Events: the session is begun on READY, ended on STOPPING; EXITING or a lost instance
quits the game; losing focus (the headset's system menu) opens the pause menu as a window losing focus does. The upscalers and
the frame generation are off in VR (an upscaler keeps one history for one view; two views a frame would need a context and a
history per eye), and the window only mirrors the left eye, without v-sync (the headset paces the frames: `xrWaitFrame`).

### Two views (`src/game/vr_play.cpp`, `main.cpp`)

Each frame the game ticks once and the scene is drawn twice, one renderer frame per eye, from the same game state: the scene
build (lights, the flashlight, effects) runs once for the head, then each eye renders with its own camera. The second eye
repeats the first eye's frame: the frame counter does not advance (the time, the temporal patterns and the ray tracing
structures are the first eye's) and the exposure adapts once (`SceneRenderer::SetVrEye`).

The renderer only draws symmetric frusta (its shaders rebuild view positions from `1/proj[0][0]` and `1/proj[1][1]`), while
headsets have asymmetric fields. Each eye is drawn with the symmetric frustum that holds its field, at the image's pixel density
(square pixels, the denser axis), and the eye's own field is cut out when the frame is copied into its swapchain image; the
projection layer then gets the eye's real field (`src/engine/xr/xr_view.h`, checked by `tests/vr_view_test.cpp`). For the
test runtime's 1024x1104 views with a 54/42/47/51 degree field that is a 1318x1182 frame, 1090x1104 pixels of it per eye.

**The game's camera stays the logic camera.** The head's yaw and pitch take the place of the look stick
(`InputState::vr_look`, `Player::UpdateLook`) while the look is the player's, so the traps, Lisa's checks and every in-view test
read where the player looks, through the game's own camera, as without VR. The eyes are drawn from the player's eye anchor (the
feet at the eye's height, eased, without the walk's head bob and the head bone's lean) plus the tracked head translation scaled
by `world_scale`: positional tracking moves the eyes up to 0.5 m sideways, 0.4 m up and 0.8 m down, and never closer than 0.12 m
to a wall of the movement collision. The game's camera roll is not used (the head rolls). When something else turns the player (a demo handing the view
back, a warp, a route's facing) `Player::UpdateLook` keeps that turn on top of the head's yaw and the VR view takes it over at
the next frame (`Player::TakeVrTurn`), turning its world once, so the game's own turns keep their meaning; while the game holds
the look (lock B) the head looks around without moving the logic camera. The world never pitches: a pitch the game sets is
dropped in favour of the head's, except an input script's facings (`Player::SetScriptPitch`), which stand in for the head's
pitch in the logic camera of a test run.

Screen effects in the eyes:

| effect | in VR |
| --- | --- |
| tonemap, bloom, colour grading (LUT), exposure, fog, SSAO, reflections, ray tracing, FXAA, banding canceller | kept, per eye |
| fades (the game's full screen fades) | kept, on the eyes (the HUD leaves them out) |
| depth of field, motion blur | off (the eye focuses itself; a blur that follows the head makes people sick) |
| film grain, lens distortion, lens flares, screen sprites | off (screen-space patterns do not belong to a view per eye) |
| full screen blur and its history (the dizzy sway) | off (its history would mix the two eyes) |
| camera roll, head bob | off (the head is the camera) |
| zoom (R3, the trigger) | the game's logic zooms (traps that need it work); the view does not narrow |

### HUD, menus and the virtual screen

The game's UI (subtitles, prompts, the pause menu, the PC settings, the loading and save icons) is drawn alone on a transparent
1920x1080 image (the UI's own blend with its coverage in alpha, premultiplied) and shown as a quad 1.4 m wide 1.6 m away, a
little below the eyes; it stays put while the head turns within 20 degrees and eases after it beyond, and comes back in front
when a menu opens. The letterbox bars and the full screen fade stay out of it.

Views the game frames itself, a demo's camera (the cinematics) and the peephole, are not drawn in stereo: forcing a camera on
the head makes people sick. They play as the flat game shows them, effects and UI included, on a virtual screen 2.8 m wide
2.5 m away, placed in front of the head when it appears, in the dark. Back in play the stereo view returns where the player
stands.

### Controls

The OpenXR actions are bound for the Oculus Touch, Valve Index, HTC Vive, Windows Mixed Reality and Khronos simple controllers:

| action | Touch / Index | Vive | WMR | the game's |
| --- | --- | --- | --- | --- |
| walk | left stick | left trackpad | left stick | left stick |
| turn (stereo) / look (virtual screen) | right stick | right trackpad | right stick | (turning) / right stick |
| interact, confirm | A | right trigger | right trigger | cross |
| back | B | right grip | right grip | circle |
| scratch (the photo) | X (Index: left A) | left trigger | left trigger | square |
| triangle | Y (Index: left B) | left grip | left grip | triangle |
| zoom | right trigger | right trackpad click | right stick click | R3 |
| pause menu | left menu (Index: left stick click) | left menu | left menu | OPTIONS |
| PC settings | left grip | left trackpad click | left stick click | View / Share / Create |
| flashlight hand | aim pose | aim pose | aim pose | |

In a menu the left stick is the D-pad. The prompts show A, B, X and Y. Turning: snap turns of 30 degrees when the right stick
passes 70 % (again after it returns under 30 %), or smooth at 90 degrees a second. Walking follows the head's direction, as the
game's walking follows the camera. Vibration goes to both controllers.

The flashlight follows the head by default: the game's own handy light model (the hand's pose about the camera, the eased aim,
the colour, cone, mask, shadow and the flashlight reflection lights) with the head as the camera. With `flashlight = 1` it is
in the chosen controller's hand: the aim pose gives its position and direction, everything else is the same model.

## Testing without a headset

There is no headset here, so the VR mode was developed against `tools/xr_test_runtime`, a headless OpenXR runtime in this
repository: a DLL and a manifest built into `build/<name>/xr_test_runtime/` (outside the folder `tools/package.py` ships),
selected for one process with `XR_RUNTIME_JSON`. It installs nothing, writes no registry key, starts no service and opens no
window. It implements OpenXR 1.0 with `XR_KHR_vulkan_enable2` for one simulated headset and a Touch controller pair,
creates the swapchain images on the game's device, plays a script of head and controller poses and inputs, writes the layers the
game submits as PNG files, and logs everything it can check (call order, handles, the frame loop, swapchain acquire, wait and
release, the layers of `xrEndFrame`, binding paths) as `VIOLATION` lines.

`tools/vr_check.py` runs the game headless with `--vr` against it and reports. On 2026-10-07, on the merged 1.0 renderer
(1360 frames, the boot, the opening and the start room, then a walk into the corridor):

- the frame loop: 1360 frames waited, begun and ended; 0 violations; the session stopped and the instance was destroyed cleanly
- the Khronos API validation layer (`XR_APILAYER_LUNARG_core_validation`, built from the same SDK release, `--layer`): no
  error or warning
- the stereo pair: the far content's offset matches the fields the runtime gave (214 px), the parallax of the start room's
  walls (+9 px at 64 mm, about 3 m) and the corridor's (+13 to +15 px, about 2 m) is right for their distance, 0 px vertical offset
- the pose path: the simulated head's yaw and pitch reach the game's camera on every stereo frame but 19 of 1277 (the boot
  frames and the demo hand-backs, where the game holds the look and turns the player; the view then follows the turn once:
  `vr: the game turned the player by 179.6 degrees, the view follows`), the left stick walked the player 12.9 m along the
  head's direction, the right stick snap-turned by 30 degrees
- the layers: the HUD quad shows the studio logo and the pause menu on a transparent image; the opening demo and the
  corridor demo play on the virtual screen
- turning: a snap turn of 30 degrees on the right stick; with `turn = 1` the stick held for one second turned 90 degrees
- focus: the runtime taking the focus away (its system menu) opened the pause menu; the menu shows on the HUD
- the PC settings page opens from the controller (left grip) on the HUD, with the Xbox button names; Extras > VR (experimental)
  and its page (VR mode, Flashlight, Turning) were also checked in the flat game with a virtual pad, and the flashlight row
  wrote `flashlight = 1` to `pt.ini`
- the flashlight: in the hand, turning the hand 25 degrees left and right moves the lit area to the left and right of the view
  (centroid of the change at x 395 and 801 of 1024); following the head, the same hand turns change nothing

`tools/walkthrough.py --vr` runs the walkthrough routes in the VR mode against the test runtime (the routes' facings and walks
turn the VR world as the game's own turns do, their pitch stands in for the head's; views 512x552 to keep two eyes a frame
quick). On 2026-10-07, on the merged renderer, 26 of the 26 scenarios it runs passed (`prompts` is left out in VR: it
checks the keyboard and PlayStation prompt textures, and the VR mode shows the controllers' A, B, X and Y instead).

What the test runtime cannot show: what a real compositor does with the layers (timing, reprojection, the quads' sharpness),
comfort, performance at a headset's rate (two full renders a frame, the shadows drawn twice), real controllers' bindings and
their buttons' feel, the runtime's own quirks (some runtimes list only Vulkan 1.1 or 1.2 in their requirements; the port asks
for 1.3 and logs a warning when the runtime's list stops below it). The runtime only confirms this reading of the specification;
the validation layer checks the calls independently of it. The VR mode is validated against these two only: no independent
runtime (Monado's null compositor was considered; its Windows builds are only on its GitLab behind a bot check, and a third-party
mirror's code was not to run in the tests) and no real headset.

## What 1.0 ships and what is not tested

The VR mode is in the 1.0 release as an experimental extra on the release renderer (merged on the 0.9.1 hotfix and Game+
work: the flare, shadow, reflection, upscaler and third person changes are the release's; the VR path only adds its two
eyes, the HUD quad and the virtual screen on top). Checked without a headset, so these hold:

- off by default and inert when off: no OpenXR call, no loader load, the render check gives 0.00 against the release build
  and the walkthrough set passes
- on without a runtime or headset: the clear log line above and the flat game, every time
- the frame loop, the layers, the stereo geometry, the head look, the controls, the HUD and the virtual screen against the
  test runtime and the Khronos validation layer (the figures above)

Not tested, because it needs a headset: the picture in a real headset (the compositor's reprojection and the quads'
sharpness), comfort (the eye height, the HUD and screen sizes and distances, the snap turn's step), performance at a
headset's rate, real controllers (the bindings are the specification's, the buttons' feel is a guess), the runtimes' own
differences (SteamVR, the Meta runtime and WMR each give their Vulkan requirements, formats and view sizes). A first run on
hardware should start at `resolution_scale = 0.7` and the Low preset and read `pt.log` for `vr:` lines.

## Cutscenes, the peephole and the zoom in VR

Chosen, not measured with people: the views the game frames itself are not forced on the head. A demo's camera (every
cinematic, the hand-backs included) and the peephole play as the flat game shows them, effects and UI included, on a virtual
screen 2.8 m wide 2.5 m away, placed in front of the head when it appears, in the dark; the head may look around it. When the
view returns to the player the stereo view comes back where the player stands and the world is turned once to match the
game's own facing. The zoom (R3, the right trigger) keeps the game's logic (the traps that read it work) but does not narrow
the eyes' field: a field change on the head is the quickest way to make people sick. The pause menu, the PC settings and
every prompt stay on the HUD quad, which follows the head lazily.

## Risks

- Performance: each eye is a full render with its own shadows, reflections and post chain. At 90 Hz on 2x1318x1182 that is
  about four times the 1080p flat game's work; `resolution_scale` and the graphics presets are the levers.
- The anchor's eased eye height and the head reach limits were chosen, not measured with people; the comfort of the HUD and the
  virtual screen sizes likewise.
- The game's scripted turns turn the VR world (once per turn); a scene that turns the player often would turn the world often.
- Upscalers and frame generation are unavailable in VR.
