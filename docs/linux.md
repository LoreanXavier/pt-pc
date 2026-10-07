# Linux

The game (`pt`) and the installer (`pt_setup_linux`) build for Linux x86-64. I develop on Windows and check the Linux
build by cross compiling; the notes below say what runs where.

## Platform layer

Everything the engine needs from the operating system beyond SDL3 goes through `src/engine/platform`:

| Piece | Windows | Linux |
|---|---|---|
| `os.h`: files with Unicode paths, 64-bit seek, environment, process id | `_wfopen`, `_fseeki64`, `_dupenv_s` | `fopen`, `fseeko`, `getenv` |
| `os.h`: `RunProcess` (texture upscaler, installer helper) | `CreateProcessW`, no console, below-normal priority | `posix_spawn`, nice 10, SIGKILL on cancel or timeout |
| `os.h`: `FileLock` (one texture generation at a time) | file opened without sharing | `flock` |
| `http.h`: HTTPS GET (update check) | WinHTTP | the system libcurl, loaded with `dlopen` (the game starts without it) |
| Unicode fonts (Turkish, Chinese, Arabic, Russian, Ukrainian, Czech UI) | GDI + Uniscribe | HarfBuzz 14.6.0 (fetched, built into the binary) + stb_truetype, a small bidi pass for Arabic lines |
| voice recognizer (whisper.cpp) | `whisper.dll`, `ggml*.dll` in `voice/` | `libwhisper.so`, `libggml*.so` in `voice/` (RPATH `$ORIGIN`), worker thread at nice 10 |
| enhanced textures | `realesrgan-ncnn-vulkan.exe` + `vcomp140.dll` | `realesrgan-ncnn-vulkan` (the project's ubuntu release, checked by SHA-256) |
| folder picker when no game is found | `IFileOpenDialog` | none: a message box asks for `--game` |
| crash dump | `MiniDumpWriteDump` | none |

Off on Linux: AMD FSR 3 and its frame generation, Intel XeSS and NVIDIA DLSS. Their SDKs ship Windows DLLs, so
`PT_UPSCALERS` defaults to OFF outside Windows and the code behind `PT_WITH_FSR`, `PT_WITH_DLSS` and `PT_WITH_XESS` is
left out. DLSS has a Linux runtime (`libnvidia-ngx-dlss.so`) that could be added later. Vulkan itself is loaded by volk
at run time; SDL3 loads X11, Wayland, ALSA, PulseAudio and PipeWire at run time.

Settings, saves and `pt.log` live in `~/.local/share/pt-port/pt/`.

## Building

Native, on a Linux machine (X11 and Wayland): `cmake -G Ninja -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo` then
`cmake --build build/linux --parallel 6 --target pt`. Needs a C++20 compiler with `<format>` (GCC 13+ or clang 17+),
the Vulkan headers and `glslc`.

Cross build on Windows:

1. `python tools/linux/make_sysroot.py` (in the folder where the sysroot should go; it reads `Packages.xz` of Debian trixie
   amd64 from deb.debian.org and verifies every package's SHA-256): headers and libraries of 87 packages, about 335 MB.
2. `PT_LINUX_SYSROOT=<sysroot> PT_DEPS=<a Windows build's _deps> sh tools/linux/cross_build.sh [target]`: LLVM clang and
   lld, CMake, Ninja, at most 6 jobs (`PT_JOBS`). The toolchain file is `cmake/toolchains/linux-x86_64-clang-cross.cmake`.
   Wayland is off in this cross build only, because SDL needs the host tool `wayland-scanner`; X11 is on.

Every target builds this way: `pt`, all unit tests and `pt_setup_linux`. `pt` needs only `libc.so.6`, `libm.so.6` and
`ld-linux-x86-64.so.2` (the C++ runtime is linked in with `-static-libstdc++ -static-libgcc`), and glibc 2.38 or
newer, because the Debian trixie headers map `strtol`, `sscanf` and `fmod` to their 2.38 versions and SDL finds
`strlcpy` there. That is Ubuntu 24.04, Debian 13, Fedora 39 and newer, and current Arch and SteamOS. Building against
an older sysroot would lower it.

## Running the checks

From a Linux machine or WSL, with the cross build in `build/linux-cross`:

    sh tools/linux/run_tests.sh <game folder> <output folder> <linux installer>

It records the environment (distribution, glibc, Vulkan device, missing libraries with the apt line to install them),
`ldd`, the unit tests (`pt_platform_test --network` covers processes, locks, seek, HTTPS through libcurl and the update
manifest), a 200 frame headless run on the default Vulkan driver and on lavapipe, two scripted routes through
`tools/walkthrough.py`, the Linux installer (self test, update check, install from the game folder, the installed game
started without `--game`), and the Windows build under Proton or Wine when one is installed. The summary is in
`summary.txt`.

## Proton

The Windows build is plain Vulkan with SDL3 and no anti-cheat or launcher, so it runs under Proton or Wine (winevulkan)
as well; FSR, DLSS (through DXVK-NVAPI) and XeSS are Windows DLLs and work there as far as Proton supports them.
`run_tests.sh` runs it when Proton or Wine is present.
