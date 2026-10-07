# VR mode (experimental)

P.T. in a VR headset through OpenXR. I do not own a headset: this mode was built and checked against a simulated
OpenXR runtime (`tools/xr_test_runtime`, in this repository) and the Khronos validation layer, never against real
hardware. Expect problems those cannot show. It is off by default, and with it off the game is unchanged.

## Turning it on

- PC settings > Extras > VR (experimental) > VR mode: On, then restart. Or `[vr] enabled = 1` in `pt.ini`, or `pt.exe --vr`
  for one run (`--no-vr` never starts it).
- It needs an OpenXR runtime with `XR_KHR_vulkan_enable2` set as the system's active runtime (SteamVR, the Meta Quest
  Link app, Windows Mixed Reality, Varjo, Pimax, Monado) and a connected headset. Without them the game logs why and
  starts flat: `vr: off for this run: xrCreateInstance ... XR_ERROR_RUNTIME_UNAVAILABLE` with no runtime, `xrGetSystem
  ... XR_ERROR_FORM_FACTOR_UNAVAILABLE` with a runtime but no headset. The setting stays on, so the next start tries again.
- The Khronos OpenXR loader `openxr_loader.dll` 1.1.63 ships next to `pt.exe` (Apache-2.0, text in `licenses/`). The build
  downloads it and checks it by SHA-256 (`cmake/OpenXR.cmake`); `-DPT_OPENXR=OFF` builds without VR. The DLL is loaded
  only when VR is on.

`pt.ini` `[vr]`:

| key | values | meaning |
| --- | --- | --- |
| `enabled` | 0, 1 | |
| `flashlight` | 0, 1 | 0 follows the head like the game's camera, 1 is held in a controller |
| `flashlight_hand` | 0, 1 | left or right controller |
| `turn` | 0, 1 | snap turns, or smooth turning |
| `snap_degrees` | 10 to 90 | default 30 |
| `smooth_speed` | 20 to 360 | degrees a second, default 90 |
| `resolution_scale` | 0.5 to 2 | eye image size relative to what the runtime recommends, default 1 |

The flashlight and the turning can also be changed on the Extras > VR page while playing. A first run on real hardware
should start at `resolution_scale = 0.7` and the Low preset, and read `pt.log` for `vr:` lines.

## What it does

Each frame the game ticks once and the scene is drawn twice, one full render per eye, from the same game state. The
runtime creates the Vulkan device, and the session gets four swapchains: one per eye, a 1920x1080 HUD and a 1920x1080
virtual screen. The window mirrors the left eye without v-sync; the headset paces the frames. Losing focus (the
headset's system menu) opens the pause menu.

The game's camera stays the logic camera. The head's yaw and pitch take the place of the look stick, so the traps,
Lisa's checks and everything else that reads where the player looks work as without VR. The eyes are drawn from the
player's eye height plus the tracked head: positional tracking moves them up to 0.5 m sideways, 0.4 m up and 0.8 m
down, and never closer than 0.12 m to a wall. When the game itself turns the player (a cutscene handing the view back,
a warp) the VR world turns once to match. The world never pitches with the game's camera; the head's pitch is the only
one.

Screen effects in the eyes:

| effect | in VR |
| --- | --- |
| tonemap, bloom, colour grading, exposure, fog, SSAO, reflections, ray tracing, FXAA | kept, per eye |
| the game's full screen fades | kept, on the eyes |
| depth of field, motion blur | off |
| film grain, lens distortion, lens flares, screen sprites | off |
| full screen blur (the dizzy sway) | off |
| camera roll, head bob | off |
| zoom (R3) | the game's logic zooms, the view does not narrow |

Upscalers and frame generation are off in VR.

Cutscenes and the peephole are not drawn in stereo, because forcing a camera on the head makes people sick. They play
as the flat game shows them, effects and UI included, on a virtual screen 2.8 m wide 2.5 m away, in the dark. The
game's UI (subtitles, prompts, the pause menu, the PC settings) is drawn on a transparent 1920x1080 quad 1.4 m wide
1.6 m away that follows the head lazily and comes back in front when a menu opens.

## Controls

OpenXR actions are bound for the Oculus Touch, Valve Index, HTC Vive, Windows Mixed Reality and Khronos simple
controllers:

| action | Touch / Index | Vive | WMR | the game's |
| --- | --- | --- | --- | --- |
| walk | left stick | left trackpad | left stick | left stick |
| turn (stereo) / look (virtual screen) | right stick | right trackpad | right stick | right stick |
| interact, confirm | A | right trigger | right trigger | cross |
| back | B | right grip | right grip | circle |
| scratch (the photo) | X (Index: left A) | left trigger | left trigger | square |
| triangle | Y (Index: left B) | left grip | left grip | triangle |
| zoom | right trigger | right trackpad click | right stick click | R3 |
| pause menu | left menu (Index: left stick click) | left menu | left menu | OPTIONS |
| PC settings | left grip | left trackpad click | left stick click | View / Share / Create |
| flashlight hand | aim pose | aim pose | aim pose | |

In a menu the left stick is the D-pad. The prompts show A, B, X and Y. Snap turns trigger when the right stick passes
70 % and arm again under 30 %. Walking follows the head's direction. Vibration goes to both controllers.

## Known limits

- Each eye is a full render with its own shadows, reflections and post chain: at 90 Hz that is about four times the
  work of the 1080p flat game. `resolution_scale` and the graphics presets are the levers.
- Eye height, head reach limits, the HUD and screen sizes and the snap step were chosen, not tested with people.
- Some runtimes list only Vulkan 1.1 or 1.2 in their requirements; the port asks for 1.3 and logs a warning.
- A scene that turns the player often would turn the VR world often.

## Testing without a headset

`tools/xr_test_runtime` is a headless OpenXR runtime: a DLL and a manifest built into `build/<name>/xr_test_runtime/`
(not shipped), selected for one process with `XR_RUNTIME_JSON`. It installs nothing and opens no window. It implements
OpenXR 1.0 with `XR_KHR_vulkan_enable2` for one simulated headset and a Touch controller pair, plays a script of head
and controller poses and inputs, writes the layers the game submits as PNG files, and logs anything it can check as
`VIOLATION` lines. `tools/walkthrough.py --vr` plays the scripted routes in VR against it. If you have a headset and
try the mode, an issue with your `pt.log` is welcome.
