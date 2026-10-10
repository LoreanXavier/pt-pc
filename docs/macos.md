# macOS

The game (`pt`) builds for macOS 14 or newer on Apple silicon (M1 and later, arm64) and on Intel Macs (x86_64). It ships as
`P.T. PC Port.app`, one app per CPU: `P.T.PC.Port-macOS-arm64.zip` and `P.T.PC.Port-macOS-x64.zip` on the Releases page.
There is no installer on macOS: the app asks for the dump folder on the first start.

The macOS version is the port's own and does what the Linux one does, with these limits: FSR 3, DLSS and XeSS (Windows
SDKs), the VR mode and the ray-traced shadows, ambient occlusion and reflections (MoltenVK has no ray queries) are off, and
the settings page greys them out. Upscaling is available through **Apple MetalFX** instead (`upscaling.md`, Apple MetalFX),
on a GPU MetalFX supports. Nothing of the macOS port could be built or run on the machine it was written on (a Windows PC);
what has been tried on a Mac is in the credits below.

## Running

1. Unzip the zip for your Mac (`...-arm64.zip` for an M1 or later, `...-x64.zip` for an Intel Mac; Apple menu > About This
   Mac says which) and move `P.T. PC Port.app` where you like (Applications, or a folder of its own). The game's own
   update notice names the right zip for the CPU it runs on.
2. The app is signed ad hoc, without a paid Developer ID or notarisation. After you try opening the downloaded app once,
   go to System Settings > Privacy & Security, scroll to Security, choose **Open Anyway**, then confirm **Open**. Do this
   only for a copy you downloaded from the project's Releases page and trust. See [Apple's instructions for opening an app
   that hasn't been notarised](https://support.apple.com/en-gb/102445).
3. On the first start a folder dialog asks for your extracted CUSA01127 folder (the one with `chunk1.psarc` and
   `texture.qar`). The choice is remembered. A `CUSA01127` or `game/CUSA01127` folder next to the app is found without
   asking, and `pt --game <folder>` (`P.T. PC Port.app/Contents/MacOS/pt`) still works. A fake PKG cannot be used on
   macOS: the extraction helper of the installers is not built for it, so extract the PKG on Windows or Linux first.
4. macOS asks once for the microphone, for the part of the game that listens for the word. `[voice] key = J` in `pt.ini`
   works here too.

Settings, the save, `pt.log` and the enhanced texture cache are in `~/Library/Application Support/pt-port/pt/`. Mods go
in `mods/` in that folder (docs/modding.md), since the app bundle is no place for user files.

## Platform layer

macOS takes the POSIX side of `src/engine/platform` (docs/linux.md), with these differences:

| Piece | macOS |
|---|---|
| Vulkan | MoltenVK 1.4.2 (Vulkan 1.4 on Metal, Apache-2.0), loaded by the game from `Contents/Frameworks/libMoltenVK.dylib` or next to `pt` in a build folder; SDL is pointed at the same file (`SDL_HINT_VULKAN_LIBRARY`). The instance asks for portability drivers and the device enables `VK_KHR_portability_subset`. `PT_VULKAN_LIBRARY` picks another library, such as the Vulkan SDK's loader for the validation layers |
| `http.h`: HTTPS GET (update check) | the system's `/usr/lib/libcurl.4.dylib`, loaded with `dlopen` as on Linux. The manifest entry is `macos-arm64` or `macos-x64` by the CPU the game was built for (`Platform()` in update_check.cpp) |
| voice recognizer (whisper.cpp) | `libwhisper.dylib`, `libggml*.dylib` and ggml's CPU variants in `voice/` (CMake modules keep `.so`), rpath `@loader_path`, worker thread at QoS utility. The variants are the target CPU's: `libggml-cpu-apple_m1.so`, `-apple_m2_m3`, `-apple_m4` on Apple silicon; `-x64`, `-sse42`, `-haswell`, `-skylakex` and the other x86 ones on an Intel Mac, each loaded only on a CPU that has what it needs. CPU only, as elsewhere: ggml's Metal and Accelerate backends are off |
| audio mixer | flush to zero through ARM64 FPCR.FZ on Apple silicon, the SSE control register (FTZ and DAZ) on Intel |
| `LiveSplitClient` | no `MSG_NOSIGNAL` on macOS: `SO_NOSIGPIPE` is set on the socket |
| window opened in the background | no pause menu before the controller's first tick (an app started from Finder or the Dock can lose the focus while it starts) |
| shadow maps | compared in the shader from a `textureGather` (`PT_SHADOW_GATHER`, shaders/lighting.glsl) instead of a comparison sampler: with one on the bindless `images[]` array SPIRV-Cross declares the whole array `depth2d`, and Metal reads the G-buffer's normal and material through it as one channel, which lit the hallway ceiling in blotches |
| enhanced textures | the `realesrgan-ncnn-vulkan` macOS release (a universal binary, x86_64 and arm64, MoltenVK linked in; checked here by its Mach-O header), by SHA-256 |
| missing GPU features | the device is checked for the features the renderer needs (`vk_context.cpp`) and the log names the one that is missing |
| folder picker when no game is found | SDL's folder dialog (`NSOpenPanel`) |
| crash dump | none |

Off on macOS, as on Linux: FSR 3, DLSS, XeSS (Windows SDKs) and the VR mode. MoltenVK has no ray queries, so the
ray-traced shadows, ambient occlusion and reflections are greyed out in the settings; Metal has no sampler LOD bias,
which MoltenVK ignores.

Upscaling on macOS is Apple MetalFX (`upscaling.md`, Apple MetalFX; `src/engine/render/upscale/metalfx_backend.mm`): the
port's temporal upscaler, run on a Metal command buffer beside MoltenVK's. It uses `VK_EXT_metal_objects` to name the
Vulkan targets' Metal textures, and splits each frame's submission in two around the Metal pass, ordered by an
`MTLSharedEvent` imported into a Vulkan timeline semaphore. `MTLFXTemporalScalerDescriptor.supportsDevice:` decides whether
a machine can run it: Apple silicon and the supported AMD GPUs, not an Intel Mac's integrated GPU, which greys the row out
with the reason. `PT_UPSCALER=metalfx` selects it for a run (upscaling.md, Test overrides). The MetalFX framework is part
of macOS; nothing of it ships with the app. Built with `-DPT_METALFX=OFF` the backend and the Objective-C++ file are left
out and the game behaves as the builds before.

Intel Macs: the renderer indexes a bindless array of images, which MoltenVK builds on Metal argument buffers tier 2.
Intel's own integrated GPUs and older AMD ones may not offer that; the game then stops at start with a log line naming the
missing Vulkan feature (`pt.log`, "required feature ... is not available"). Which Intel Macs run it has not been tried.

## Building

Xcode's command line tools (`xcode-select --install`), then from Homebrew `cmake ninja shaderc` (for `glslc`),
`vulkan-headers` and `vulkan-loader` (CMake's FindVulkan wants a library; the game itself loads MoltenVK):

    cmake -G Ninja -B build/macos -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build/macos --target pt

The build is for the CPU of the Mac it runs on (arm64 or x86_64; `-DCMAKE_OSX_ARCHITECTURES=` names it). A universal build
is refused: ggml's CPU variants for the voice recognizer are chosen per architecture. Build once on each kind of Mac, as the
workflow does; `make_app.py` reads the architecture from the executable and names the zip `...-macos-arm64.zip` or
`...-macos-x64.zip`.

CMake downloads MoltenVK and the macOS Real-ESRGAN build next to the other dependencies (cmake/MacOS.cmake,
cmake/EnhancedTextures.cmake). `build/macos/pt` runs from the build folder. The unit tests build with the default
target, as elsewhere.

    python3 tools/macos/make_app.py --build build/macos

makes `dist/P.T. PC Port.app` (MoltenVK in `Contents/Frameworks`, the shaders, fonts, voice runtime and texture tools in
`Contents/Resources`, where `SDL_GetBasePath()` points in a bundle), signs every binary in it ad hoc and zips it with
`ditto`. `--exe build/macos/pt_release` packages the release game; with `--game <folder>` the bundled game also shoots
the loop browser's previews, as tools/package.py does.

## Releases

`.github/workflows/macos.yml` runs when a release is published, on GitHub's Apple silicon runner (`macos-26`, arm64) and
Intel runner (`macos-15-intel`, x86_64): each builds `pt` and `pt_release` at the release's version natively and makes the
app with `tools/macos/make_app.py`. A last job attaches `P.T.PC.Port-macOS-arm64.zip` and `P.T.PC.Port-macOS-x64.zip` to the
release. The game discovers these assets directly through the GitHub Releases API (docs/updates.md). Started by hand
(Actions > macOS > Run workflow) it leaves both zips as workflow artifacts, for a test. Without game files on the runner the
zips have no loop browser previews; the game shoots them at the browser's first use. GitHub retires its Intel runners in
2027; after that the Intel app needs another Intel Mac or a `CMAKE_OSX_ARCHITECTURES=x86_64` cross build on the arm64 runner
(not tried).

## Credits and what was tried

- Apple silicon: ahm3texe (LoreanXavier/pt-pc pull request 4), M4, macOS 27: the 22 unit tests and the 28 walkthrough
  scenarios; the bundle played the first room and the hallway. Not tried on M1 to M3 or on an older macOS.
- ignusloki (github.com/ignusloki/pt-pc, `codex/macos-arm64-port`, preview 1 to 3): Apple silicon, M1 Pro. From that fork
  come the `SO_NOSIGPIPE` socket option, the check for missing GPU features, enabling the portability subset's features and
  the no-pause-before-the-first-tick fix. Their native macOS installer and the Fast Walk option are not part of this port.
  That fork has no Intel build; the Intel support here is derived from the same code and has not been run on an Intel Mac.
