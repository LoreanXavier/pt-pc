# Controller feedback

The PC settings page includes a vibration mode. **Original** is the default and keeps the game's standard motor vibration.
**Enhanced** also enables trigger vibration on compatible controllers and, on supported wired USB DualSense controllers
on Windows, adds vibration derived from Lisa's filtered crying audio. Enhanced effects depend on the controller and its
driver; unsupported effects stay unavailable. Turning the main Vibration setting off disables both original and enhanced
vibration.

Controller-speaker playback plays Lisa's authored crying audio through supported Sony controllers. DualShock 4 speaker
playback starts automatically over Bluetooth and is experimental. DualSense keeps its separate optional setting, off by
default. Speaker volume has its own control for both controllers. The normal game mix,
including headphone output, continues unchanged, and speaker volume does not change vibration strength. The speaker
setting and its volume are independent of the vibration mode. The original game plays nothing through the controller speaker
(`gameplay.md` 14.5), so this stays a PC addition. Setting the speaker volume to zero mutes either controller.

DualSense uses its four-channel USB audio endpoint. DualShock 4 uses its physical Bluetooth HID interface: the game's PCM is
resampled to 32 kHz, encoded as SBC and sent to the internal speaker using report 0x17, target 0x02. A speaker-only volume
command arms the route; closing it mutes the speaker. Rumble, lightbar, headset and microphone controls are not overwritten.
USB audio on a DS4 feeds its headset jack; it is not an internal-speaker route. A USB-only DS4 therefore logs a Bluetooth
requirement instead of opening that jack or an unrelated PC sound device. See [DS4-BT](https://www.psdevwiki.com/ps4/DS4-BT) for the Bluetooth protocol.

Finding the DualSense speaker: on Windows the pad's playback endpoint is looked up through its PnP container. If that finds
none, or on Linux and macOS, the one four-channel playback device named "DualSense" is used. Missing or ambiguous devices
leave the speaker off; the main mix stays unchanged. DS4 Bluetooth matching uses its physical HID path or a single physical
BT DS4 behind a virtual pad. Unsupported or failed routes retry at most every 2 s. The route follows the last-used controller,
so unplugging and reconnecting works. `PT_CONTROLLER_SPEAKER_DEVICE=<part of a playback device name>` names a stand-in endpoint
for software tests; DS4 USB headset endpoints are accepted only through that explicit override.
With `--virtual-pads` the route opens only with that override and SDL's real controller backends stay off, so test runs never reach a
real pad (a virtual DualSense once matched the real one's endpoint by name and played the tests' haptics on it).

The DualSense's speaker and its headphone jack share the endpoint's first channel pair; the pad plays it on the jack unless USB
output report 0x02 selects the speaker. The game selects the speaker only while the speaker setting is on and the game's main
mix plays on another device, and only the controller speaker events go there. When the main output is the pad itself (a USB
DualSense often becomes the Windows default device, see Sound > Output device), the pad stays on its jack and the speaker PCM is
muted; the speaker row's note says so. A speaker path the game selected goes back to the jack when the route closes. 1.0.7
selected the speaker whenever the route opened and never changed it back: with the pad as the Windows default the speaker
played the whole game (`docs/dualsense-output-2026-10-20.md`).

These controller features have passed software tests for supported-capability handling, event selection and audio
routing. The DS4 Bluetooth encoder, packet layout, CRC and physical-device matching have software coverage, but audible playback
on a physical DS4 has not been verified. DualSense speaker audio still requires USB. Trigger vibration and DualSense actuator
output have not been verified on hardware either.

PC settings > Accessibility has two more controller features, both off by default. **Sound captions** give the controller a short
rumble pulse (the small motor, a quarter of a second) with a caption for sounds that have no vibration of their own, when
Vibration is on; they do not change the rumble modes above. **Gyro** (Camera or Flashlight) reads the motion sensor of a
DualSense, DualShock 4 or Switch Pro controller through SDL; L3 recentres the flashlight. Both are described in
`docs/gameplay.md` (14.8 and 14.9). The gyro has passed software tests with virtual controllers only; real motion sensors have
not been tested.

## Start-up

The controllers and the audio devices start on threads of their own in a window on Windows (`src/engine/platform/device_start.h`);
the window, the graphics device and the boot screens never wait for them. A 1.0.2 player's start sat 98 s inside `SDL_Init` before
any window (a wired DualSense Edge, a TV on HDMI among the audio devices) and a 1.0.3 player's 5.4 s in the gamepad subsystem
alone with no pad connected: SDL enumerates every device there (DirectInput, which SDL itself notes can take a minute even with
no controllers; HIDAPI's open of each HID device; the WASAPI endpoints).

- Controllers: their thread stays to receive HIDAPI's device change messages (SDL makes that window on the thread that starts the
  subsystem), so plugging a pad in or out works as before. Until the start is over SDL's own pad update in the event pump is off
  (`SDL_HINT_AUTO_UPDATE_JOYSTICKS`, it would wait on the joystick lock the start holds) and Dear ImGui is not asked for pads;
  then the pads connected are opened in that frame. DirectInput's enumeration is timed first on its own: not over in 2 s, SDL
  starts without DirectInput that session (XInput and HIDAPI pads as usual; SDL would otherwise repeat the slow enumeration on
  the main thread at every device change), and the log says so.
- Audio: the sound waits at most 0.25 s for the audio start when it is set up; still starting, it begins without a device (as on a
  machine with none) and opens the device in the frame the start ends.
- Headless runs, virtual pads (`--virtual-pads`) and Linux and macOS start both on the main thread, timed the same way.

pt.log has a `boot:` line for each phase: the main thread's phases (data folder, crash reporting, game folder with where it was
found, game archives, video, settings, window, mods and graphics settings, graphics device, game data, sound, first frame with
what it shows) each timed from the end of the one before and the first from the process's creation, so they add up to the time
since the start; the background ones (DirectInput enumeration, controllers, audio devices) give their own length. Slow 1.0.3
starts with 5 to 22 s before `game files found at` were first starts with the game in no folder the start searches: the time
was the player's in the folder dialog, which the log now states (the boot line's `a folder picked in the first-run screen
after 11.2 s`). Test switches
`PT_TEST_SLOW_GAMEPAD_MS`, `PT_TEST_SLOW_AUDIO_MS` and `PT_TEST_SLOW_DIRECTINPUT_MS` slow those starts down (the controllers' with
the joystick lock held); `pt_device_start_test` and `tools/boot_timing_check.py` (focus-free windowed runs) use them.
