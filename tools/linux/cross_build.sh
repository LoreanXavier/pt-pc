#!/bin/sh
# Cross builds the Linux x86-64 game on Windows (Git Bash) with LLVM clang/lld and a Debian sysroot (docs/linux.md).
#   PT_LINUX_SYSROOT=C:/Projects/pt-linux-env/sysroot PT_DEPS=C:/Projects/pt-port/build/release/_deps tools/linux/cross_build.sh [target]
# Wayland is off in this cross build only: SDL needs the host tool wayland-scanner, which has no Windows build. A native
# Linux build (cmake -G Ninja -B build/linux) has X11 and Wayland.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
: "${PT_LINUX_SYSROOT:?set PT_LINUX_SYSROOT to the sysroot made by tools/linux/make_sysroot.py}"
CMAKE=${CMAKE:-"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"}
export PATH="/c/Program Files/LLVM/bin:${VULKAN_SDK:-/c/VulkanSDK/1.4.357.0}/Bin:$PATH"
BUILD="$REPO/build/linux-cross"
set --  "$@"
SHARED=""
if [ -n "$PT_DEPS" ] && [ -d "$PT_DEPS/sdl3-src" ]; then
  for dep in sdl3 zlib whisper volk vma glm imgui stb lua51 ogg vorbis bc7enc; do
    up=$(echo "$dep" | tr a-z A-Z)
    [ -d "$PT_DEPS/$dep-src" ] && SHARED="$SHARED -DFETCHCONTENT_SOURCE_DIR_$up=$PT_DEPS/$dep-src"
  done
fi
SCANNER="$REPO/tools/linux/wayland-scanner.cmd"
if [ -n "$PT_NO_WAYLAND" ]; then
  WAYLAND="-DSDL_WAYLAND=OFF"
else
  if ! cmd //c "$(cygpath -w "$SCANNER")" --version >/dev/null 2>&1; then
    echo "error: wayland-scanner does not run in WSL (apt install libwayland-bin in the distribution), or set PT_NO_WAYLAND=1 for an X11-only build" >&2
    exit 1
  fi
  WAYLAND="-DWAYLAND_SCANNER=$SCANNER"
fi
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  "$CMAKE" -G Ninja -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchains/linux-x86_64-clang-cross.cmake" -DPT_LINUX_SYSROOT="$PT_LINUX_SYSROOT" \
    -DSDL_KMSDRM=OFF $WAYLAND $SHARED
fi
# SDL records the library names it dlopens at configure time: they must be versioned sonames (libX11.so.6, not libX11.so, which
# only the -dev packages have) and the Wayland, PulseAudio, PipeWire and ALSA backends must be in
SDLCFG=$(ls "$BUILD"/_deps/sdl3-build/include-config-*/build_config/SDL_build_config.h | head -n 1)
if grep -E '_DYNAMIC[A-Z_0-9]* "[^"]*\.so"' "$SDLCFG"; then
  echo "error: SDL would dlopen unversioned library names (lines above): wrong sysroot or stale $BUILD" >&2
  exit 1
fi
for backend in VIDEO_DRIVER_X11 AUDIO_DRIVER_ALSA AUDIO_DRIVER_PULSEAUDIO AUDIO_DRIVER_PIPEWIRE ${PT_NO_WAYLAND:-VIDEO_DRIVER_WAYLAND}; do
  [ "$backend" = 1 ] && continue
  grep -q "^#define SDL_$backend 1" "$SDLCFG" || { echo "error: SDL_$backend is not built in ($SDLCFG)" >&2; exit 1; }
done
"$CMAKE" --build "$BUILD" --parallel ${PT_JOBS:-6} ${1:+--target "$1"}
echo "BUILD_OK $BUILD"
