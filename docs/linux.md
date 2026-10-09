# Linux

The game (pt) and the installer build for Linux x86-64. The build is checked by cross compiling on Windows; running it
needs a Linux machine or WSL (status below).

## Platform layer

Everything the engine needs from the operating system beyond SDL3 goes through `src/engine/platform`:

| Piece | Windows | Linux |
|---|---|---|
| `os.h`: files with Unicode paths, 64-bit seek, environment, process id | `_wfopen`, `_fseeki64`, `_dupenv_s` | `fopen`, `fseeko`, `getenv` |
| `os.h`: `RunProcess` (texture upscaler, installer helper) | `CreateProcessW`, no console, below-normal priority | `posix_spawn`, nice 10, SIGKILL on cancel or timeout |
| `os.h`: `FileLock` (one texture generation at a time) | file opened without sharing | `flock` |
| `http.h`: HTTPS GET (update check) | WinHTTP | the system libcurl, loaded with `dlopen` (the game starts without it) |
| `update_check.h` | shared | shared |
| the setup's integrity stamp (`self_integrity.h`) | BCrypt | a built-in SHA-256 (checked against the FIPS vectors), `/proc/self/exe` |
| Unicode fonts (Turkish, Chinese, Arabic, Russian, Ukrainian, Czech, Polish UI) | GDI + Uniscribe (`unicode_font.cpp`) | HarfBuzz 14.6.0 (fetched, built into the binary) + stb_truetype (`unicode_font_harfbuzz.cpp`), a small bidi pass for Arabic lines |
| voice recognizer (whisper.cpp, `formats/voice.md`) | `whisper.dll`, `ggml*.dll` in `voice/`, `LoadLibraryExW` | `libwhisper.so`, `libggml*.so` in `voice/` (RPATH `$ORIGIN`), `dlopen`; worker thread at nice 10 |
| enhanced textures | `realesrgan-ncnn-vulkan.exe` + `vcomp140.dll` | `realesrgan-ncnn-vulkan` (the project's ubuntu release, SHA-256 d0e8e1cf...) |
| game folder search next to the executable | yes | yes (`pt::ExecutableDir`) |
| folder picker when no game is found | `IFileOpenDialog` | none: a message box asks for `--game` |
| crash dump | `MiniDumpWriteDump` | none |

Off on Linux: AMD FSR 3 and its frame generation, Intel XeSS and NVIDIA DLSS. Their SDKs ship Windows DLLs (and the DLSS
link library used is the Windows one), so `PT_UPSCALERS` defaults to OFF outside Windows and the code already behind
`PT_WITH_FSR`, `PT_WITH_DLSS` and `PT_WITH_XESS` is left out. DLSS has a Linux runtime (`libnvidia-ngx-dlss.so`) that could
be added later. Vulkan itself is loaded by volk at run time; SDL3 loads X11, Wayland, ALSA, PulseAudio and PipeWire at
run time, by versioned soname (see "SDL and the library names" below).

## Building

Native, on a Linux machine (X11 and Wayland): `cmake -G Ninja -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo` then
`cmake --build build/linux --parallel 6 --target pt`. Needs a C++20 compiler with `<format>` (GCC 13+ or clang 17+),
the Vulkan headers and `glslc`.

Cross build on Windows (what is checked here):

1. `python tools/linux/make_sysroot.py` (in the folder where the sysroot should go; it reads `Packages.xz` of Debian trixie
   amd64 from deb.debian.org and verifies every package's SHA-256): headers and libraries of 87 packages, about 335 MB.
2. `PT_LINUX_SYSROOT=<sysroot> PT_DEPS=<a Windows build's _deps> sh tools/linux/cross_build.sh [target]`: LLVM clang and
   lld from `C:\Program Files\LLVM`, the Visual Studio CMake, Ninja, at most 6 jobs (`PT_JOBS`). The toolchain file is
   `cmake/toolchains/linux-x86_64-clang-cross.cmake`. SDL's Wayland backend needs the host tool `wayland-scanner`,
   which has no Windows build: `tools/linux/wayland-scanner.cmd` runs the one in WSL (`apt install libwayland-bin` in the
   distribution; `PT_WSL_DISTRO` names it, default Ubuntu-24.04). `PT_NO_WAYLAND=1` builds X11 only. After configuring,
   `cross_build.sh` stops when SDL would dlopen an unversioned name or lacks the X11, Wayland, PulseAudio, PipeWire or
   ALSA backend. An existing `build/linux-cross` keeps its old SDL settings: delete it after changing the sysroot.

Result (6 October 2026): every target builds, `pt`, all unit tests and `pt_setup_linux`. `pt` needs only
`libc.so.6`, `libm.so.6` and `ld-linux-x86-64.so.2` (the C++ runtime is linked in with `-static-libstdc++
-static-libgcc`), and glibc 2.38 or newer, because the Debian trixie headers map `strtol`, `sscanf` and `fmod` to their
2.38 versions and SDL finds `strlcpy` there. That is Ubuntu 24.04, Debian 13, Fedora 39 and newer, and current Arch and
SteamOS. Building against an older sysroot would lower it.

## SDL and the library names (issue #5)

1.0.1 exited at once on Fedora, Bazzite and others with `SDL_Init: No available video device`. Cause (measured in the
cross build's `SDL_build_config.h`): SDL3 records the file name `find_library` returns for each library it loads at run
time, and the sysroot holds copies of files, not symlinks, so that name was the unversioned `libX11.so`, `libXext.so`,
`libasound.so`, `libusb-1.0.so`, which only exist with the `-dev` packages. Systems with only `libX11.so.6` could not load
X11; `libasound.so` failing made the whole `SDL_Init` fail. Two more faults in the same build: Wayland was off (so a
session without XWayland had no video device at all), and PulseAudio and PipeWire were not built in, because the sysroot
lacked `libpulse.so.0` and the pkgconf call failed on Windows (a `:` in `PKG_CONFIG_LIBDIR`, and pkgconf's prefix
redefinition). The microphone and the sound output both went through ALSA only, by that unversioned name.

Now: `make_sysroot.py` adds `libpulse0` and the wayland client, cursor and egl libraries; the toolchain file points SDL's
cache variables (`X11_LIB`, `ASOUND_LIB`, `PULSE_LIB`, ...) at the versioned files, so the binary tries `libX11.so.6`,
`libXext.so.6`, `libXcursor.so.1`, `libXi.so.6`, `libXfixes.so.3`, `libXrandr.so.2`, `libXss.so.1`, `libXtst.so.6`,
`libwayland-client.so.0`, `libwayland-egl.so.1`, `libwayland-cursor.so.0`, `libxkbcommon.so.0`, `libdecor-0.so.0`,
`libpulse.so.0`, `libpipewire-0.3.so.0`, `libasound.so.2`, `libusb-1.0.so.0`. Video drivers: wayland, x11, offscreen, dummy;
audio: pipewire, pulseaudio, alsa, disk, dummy. KMSDRM is off (it needs libgbm at link time).

The log now says what SDL has: on a failed `SDL_Init`, the error, the video drivers built in, the SDL_VIDEO_DRIVER hint,
DISPLAY, WAYLAND_DISPLAY and XDG_SESSION_TYPE; at start `video: SDL driver x11 (built in: ...)`; when the audio output or
the microphone opens, `audio: SDL driver pulseaudio (built in: ...), playback devices: ...` and the recording devices.
A failed audio or gamepad backend no longer ends the game (video alone is required).

Checked in WSL1 (Ubuntu 24.04, only versioned libs, Xvfb, weston headless, PulseAudio with a sine source): the 1.0.1 binary
exits with `SDL_Init: No available video device`; with symlinks named `libX11.so`... in `LD_LIBRARY_PATH` it gets past video
and fails on `libasound.so`. The new binary logs `video: SDL driver x11` under Xvfb and `wayland` under weston, and records
from PulseAudio (`--voice-listen 2`: `microphone: recording at 16000 Hz`, recording devices listed). PipeWire loads and
fails only for lack of a server (`Pipewire: Failed to connect hotplug detection context`; no daemon runs on WSL1), so its
capture is not proven here. WSL1 has no GPU: the run ends in llvmpipe after the swapchain is created.

The Bazzite 44 report in issue #27 is the same failure from the old 1.0.1 Linux binary: its log ends at 0.002 s with
`SDL_Init: No available video device`, before SDL creates a window or Vulkan starts. That binary predates the versioned
soname and expanded SDL diagnostics described above, so the log cannot tell which backend library failed on that host.
The matching 1.0.1 startup issue is separate from a later black window: after SDL can create a window, some window
managers focus it only after it maps. Focus loss now opens the pause menu only after the window has first received focus.

Issue #22 reports the pointer escaping the game window on COSMIC. Gameplay uses SDL relative mouse mode; if that fails,
the game now logs the SDL driver and error and tries SDL's window mouse grab to confine the pointer. A failure of both
modes is logged instead of being treated as captured. This fallback is not yet checked on COSMIC, XWayland or Gamescope.
## Running the checks

WSL1 Ubuntu 24.04 is installed and starts on this PC (checked as Ubuntu 24.04.5); its `wayland-scanner` is 1.22.0. WSL1
has no GPU, so Vulkan falls back to llvmpipe. The existing WSL distribution and Linux cross-build are in place, but the
Windows system drive currently has less than 1 GB free; defer the full installer and walkthrough run below until there is
more space. Once there is room, from WSL:

    sh tools/linux/run_tests.sh /mnt/c/Projects/pt-port/game/CUSA01127 ~/pt-linux-test /mnt/d/pt-platform-out/P.T.PC.Port.Setup-linux

It records the environment (distribution, glibc, Vulkan device, missing libraries with the apt line to install them),
`ldd`, the unit tests (`pt_platform_test --network` covers processes, locks, seek, HTTPS through libcurl and the update
manifest), a 200 frame headless run on the default Vulkan driver and on lavapipe, the `early` and `f060` walkthroughs, the Linux installer (self test, update check, install from the game folder, the
installed game started without `--game`), and the Windows build under Proton or Wine when one is installed. The summary is
in `summary.txt`.

Release 1.0.0 (7 October 2026, WSL1 Ubuntu 24.04, no GPU): `release.py --linux-setup P.T.PC.Port.Setup-linux` made the
setup (184.7 MiB, payload 180.8 MiB; the game binary keeps its debug information, 116 MiB). The setup needs only libc and
libm; `--self-test` passes; `--check-update` reads the manifest and reports version 1.0.0 with nothing newer; `--install`
from the game folder passes in 7 s; the installed game logs `pt-port version 1.0.0 (linux)` and then crashes in llvmpipe
(a WSL1 limit, docs above).

## Proton

The Windows build is plain Vulkan with SDL3 and no anti-cheat or launcher, so it is expected to run under Proton or Wine
(winevulkan) without changes; FSR, DLSS (through DXVK-NVAPI) and XeSS are Windows DLLs and would work there as far as
Proton supports them. Not yet measured; the available WSL1 environment has no GPU or Proton. `run_tests.sh` runs it when
Proton or Wine is present.
