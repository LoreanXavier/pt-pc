#!/bin/sh
# Cross builds the Linux x86-64 game on Windows (Git Bash) with LLVM clang/lld and a Debian sysroot (docs/linux.md).
#   PT_LINUX_SYSROOT=<sysroot> PT_DEPS=<a Windows build's _deps> tools/linux/cross_build.sh [target]
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
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  "$CMAKE" -G Ninja -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchains/linux-x86_64-clang-cross.cmake" -DPT_LINUX_SYSROOT="$PT_LINUX_SYSROOT" \
    -DSDL_WAYLAND=OFF $SHARED
fi
"$CMAKE" --build "$BUILD" --parallel ${PT_JOBS:-6} ${1:+--target "$1"}
echo "BUILD_OK $BUILD"
