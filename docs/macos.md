# macOS

The game (`pt`) builds for macOS 14 or newer on Apple silicon (M1 and later, arm64). It ships as `P.T. PC Port.app`.
There is no installer on macOS: the app asks for the dump folder on the first start.

## Running

1. Unzip `pt-port-<version>-macos-arm64.zip` and move `P.T. PC Port.app` where you like (Applications, or a folder of
   its own).
2. The app is signed ad hoc, not with a Developer ID, so Gatekeeper stops the first start of a downloaded copy. Open it
   once, then System Settings > Privacy & Security > Open Anyway; or once in Terminal:
   `xattr -dr com.apple.quarantine "P.T. PC Port.app"`.
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
| `http.h`: HTTPS GET (update check) | the system's `/usr/lib/libcurl.4.dylib`, loaded with `dlopen` as on Linux. The release manifest has no `macos` entry yet, so the check finds nothing |
| voice recognizer (whisper.cpp) | `libwhisper.dylib`, `libggml*.dylib` and ggml's Apple silicon CPU variants (`libggml-cpu-apple_m1.so`, `-apple_m2_m3`, `-apple_m4`; CMake modules keep `.so`) in `voice/`, rpath `@loader_path`, worker thread at QoS utility. CPU only, as elsewhere: ggml's Metal and Accelerate backends are off |
| audio mixer | flush to zero through ARM64 FPCR.FZ instead of the SSE control register |
| shadow maps | compared in the shader from a `textureGather` (`PT_SHADOW_GATHER`, shaders/lighting.glsl) instead of a comparison sampler: with one on the bindless `images[]` array SPIRV-Cross declares the whole array `depth2d`, and Metal reads the G-buffer's normal and material through it as one channel, which lit the hallway ceiling in blotches |
| enhanced textures | the `realesrgan-ncnn-vulkan` macOS release (universal, MoltenVK linked in), checked by SHA-256 |
| folder picker when no game is found | SDL's folder dialog (`NSOpenPanel`) |
| crash dump | none |

Off on macOS, as on Linux: FSR 3, DLSS, XeSS (Windows SDKs) and the VR mode. MoltenVK has no ray queries, so the
ray-traced shadows, ambient occlusion and reflections are greyed out in the settings; Metal has no sampler LOD bias,
which MoltenVK ignores.

## Building

Xcode's command line tools (`xcode-select --install`), then from Homebrew `cmake ninja shaderc` (for `glslc`),
`vulkan-headers` and `vulkan-loader` (CMake's FindVulkan wants a library; the game itself loads MoltenVK):

    cmake -G Ninja -B build/macos -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build/macos --target pt

CMake downloads MoltenVK and the macOS Real-ESRGAN build next to the other dependencies (cmake/MacOS.cmake,
cmake/EnhancedTextures.cmake). `build/macos/pt` runs from the build folder. The unit tests build with the default
target, as elsewhere.

    python3 tools/macos/make_app.py --build build/macos

makes `dist/P.T. PC Port.app` (MoltenVK in `Contents/Frameworks`, the shaders, fonts, voice runtime and texture tools in
`Contents/Resources`, where `SDL_GetBasePath()` points in a bundle), signs every binary in it ad hoc and zips it with
`ditto`. `--exe build/macos/pt_release` packages the release game; with `--game <folder>` the bundled game also shoots
the loop browser's previews, as tools/package.py does.

## Releases

`.github/workflows/macos.yml` runs on GitHub's Apple silicon runners when a release is published: it builds `pt` and
`pt_release` at the release's version, makes the app with `tools/macos/make_app.py`, attaches it to the release as
`P.T.PC.Port-macOS.zip` and adds its `macos` entry to the release's `latest.json` (docs/updates.md). Started by hand
(Actions > macOS > Run workflow) it builds the same zip as a workflow artifact, for a test. Without game files on the
runner the zip has no loop browser previews; the game shoots them at the browser's first use.
