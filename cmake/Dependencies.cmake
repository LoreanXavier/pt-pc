include(FetchContent)
cmake_policy(SET CMP0169 OLD)
set(FETCHCONTENT_QUIET ON)

find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(Threads REQUIRED)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.2 GIT_SHALLOW TRUE)

FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG 1.4.350 GIT_SHALLOW TRUE)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.3 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b-docking GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_Declare(lua51 URL https://www.lua.org/ftp/lua-5.1.5.tar.gz
  URL_HASH SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333)

if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSROOT)
  # SDL loads X11, ALSA, PulseAudio, ... at run time by the name FindLibraryAndSONAME derives from the REALPATH of
  # libXext.so (-> libXext.so.6.4.0 gives libXext.so.6). make_sysroot.py stores symlinks as copies, so in the sysroot
  # libXext.so is a plain file and SDL would dlopen "libXext.so", a name only the -dev packages install: on a player's
  # system SDL_Init then fails with "No available video device" (and "Failed loading libasound.so" for audio).
  # Hand SDL the versioned files so it records the run time names.
  foreach(lib X11 Xext Xcursor Xi Xfixes Xrandr Xrender Xss Xtst asound pulse pipewire-0.3 jack sndio
              wayland-client wayland-egl wayland-cursor xkbcommon decor-0 drm gbm udev fribidi thai)
    string(TOUPPER "${lib}" var)
    string(REGEX REPLACE "-" "_" var "${var}")
    file(GLOB versioned "${CMAKE_SYSROOT}/usr/lib/x86_64-linux-gnu/lib${lib}.so.[0-9]*")
    list(FILTER versioned INCLUDE REGEX "/lib${lib}\\.so\\.[0-9]+$")
    if(versioned AND NOT ${var}_LIB)
      list(GET versioned 0 versioned)
      set(${var}_LIB "${versioned}" CACHE FILEPATH "run time name of lib${lib} for SDL's dynamic loading")
    endif()
  endforeach()
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl3 zlib)

# whisper.cpp (MIT) for the true end's word (formats/voice.md), CPU only. pt.exe stays x86-64 SSE2: whisper and ggml
# are DLLs in voice/ that VoiceRecognizer loads at run time, and the CPU code is ggml's per-CPU variants
# (ggml-cpu-x64, -sse42, -sandybridge, -haswell, -skylakex, ... DLLs), of which ggml loads the best the CPU runs.
FetchContent_Declare(whisper URL https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.4.tar.gz
  URL_HASH SHA256=57e280cee375ab02425b806ad5146b99f6eb9357e3c2b31357c8a6af2e2e44ae)
FetchContent_Populate(whisper)
set(PT_VOICE_DIR ${CMAKE_BINARY_DIR}/voice)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL ON CACHE BOOL "" FORCE)
set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS ON)
add_subdirectory(${whisper_SOURCE_DIR} ${whisper_BINARY_DIR} EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
add_custom_target(pt_voice_runtime)
add_dependencies(pt_voice_runtime whisper)
# whisper.cpp names its own output folder (bin); the DLLs go to voice/ next to the models
foreach(lib whisper ggml ggml-base)
  set_target_properties(${lib} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PT_VOICE_DIR} LIBRARY_OUTPUT_DIRECTORY ${PT_VOICE_DIR})
  if(NOT WIN32)
    # plain libwhisper.so files (no version symlinks to package) that find each other in voice/
    set_property(TARGET ${lib} PROPERTY VERSION)
    set_property(TARGET ${lib} PROPERTY SOVERSION)
    set_target_properties(${lib} PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()
endforeach()
# ggml gives clang-cl only the MSVC /arch switch of a variant, which leaves out the instruction sets its intrinsics
# need; each variant DLL is loaded only on a CPU that has them all (its ggml_backend_score)
set(PT_GGML_VARIANT_FLAGS
  "sse42|-msse4.2"
  "sandybridge|-msse4.2 -mavx"
  "haswell|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2"
  "skylakex|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2 -mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw"
  "cannonlake|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2 -mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw -mavx512vbmi"
  "cascadelake|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2 -mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw -mavx512vnni"
  "icelake|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2 -mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw -mavx512vbmi -mavx512vnni"
  "alderlake|-msse4.2 -mavx -mavx2 -mfma -mf16c -mbmi2 -mavxvnni")
foreach(variant x64 sse42 sandybridge ivybridge piledriver haswell skylakex cannonlake cascadelake icelake cooperlake zen4 alderlake sapphirerapids)
  if(TARGET ggml-cpu-${variant})
    add_dependencies(pt_voice_runtime ggml-cpu-${variant})
    set_target_properties(ggml-cpu-${variant} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PT_VOICE_DIR} LIBRARY_OUTPUT_DIRECTORY ${PT_VOICE_DIR})
  endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
  # the SSE and AVX paths of arch/x86/quants.c pass block pointers as char pointers, an error by default in clang 20
  foreach(variant x64 sse42 sandybridge haswell skylakex cannonlake cascadelake icelake alderlake)
    if(TARGET ggml-cpu-${variant})
      target_compile_options(ggml-cpu-${variant} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>)
    endif()
  endforeach()
  foreach(entry ${PT_GGML_VARIANT_FLAGS})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 variant)
    list(GET parts 1 flags)
    if(TARGET ggml-cpu-${variant})
      separate_arguments(flags NATIVE_COMMAND "${flags}")
      list(TRANSFORM flags PREPEND "/clang:")
      target_compile_options(ggml-cpu-${variant} PRIVATE ${flags})
    endif()
  endforeach()
endif()
set(PT_WHISPER_INCLUDE_DIRS ${whisper_SOURCE_DIR}/include ${whisper_SOURCE_DIR}/ggml/include)

# The models next to pt.exe in voice/: Whisper base.en (OpenAI, MIT) quantized to q5_1 by the whisper.cpp project, and
# the Silero VAD v6.2.0 (MIT) in ggml form; the notices go with them
foreach(entry
    "ggml-base.en-q5_1.bin|https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en-q5_1.bin|4baf70dd0d7c4247ba2b81fafd9c01005ac77c2f9ef064e00dcf195d0e2fdd2f"
    "ggml-silero-v6.2.0.bin|https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin|2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987")
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 url)
  list(GET parts 2 hash)
  set(target ${PT_VOICE_DIR}/${name})
  if(EXISTS ${target})
    file(SHA256 ${target} existing)
  else()
    set(existing "")
  endif()
  if(NOT existing STREQUAL hash)
    file(DOWNLOAD ${url} ${target} EXPECTED_HASH SHA256=${hash} STATUS pt_voice_status)
    list(GET pt_voice_status 0 pt_voice_code)
    if(NOT pt_voice_code EQUAL 0)
      file(REMOVE ${target})
      message(FATAL_ERROR "cannot download the voice model ${name}: ${pt_voice_status}")
    endif()
  endif()
endforeach()
configure_file(${whisper_SOURCE_DIR}/LICENSE ${PT_VOICE_DIR}/licenses/whisper.cpp-MIT.txt COPYONLY)
file(COPY ${CMAKE_SOURCE_DIR}/third_party/voice_notices/ DESTINATION ${PT_VOICE_DIR}/licenses)
foreach(stale cmudict-en-us.dict en-us vosk)
  file(REMOVE_RECURSE ${PT_VOICE_DIR}/${stale})
endforeach()
FetchContent_Populate(volk)
FetchContent_Populate(vma)
FetchContent_Populate(glm)
FetchContent_Populate(imgui)
FetchContent_Populate(lua51)
FetchContent_Populate(stb)

FetchContent_Declare(bc7enc URL https://codeload.github.com/richgel999/bc7enc_rdo/zip/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0)
FetchContent_Populate(bc7enc)
add_library(pt_bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.cpp)
target_include_directories(pt_bc7enc PUBLIC ${bc7enc_SOURCE_DIR})
if(MSVC)
  target_compile_options(pt_bc7enc PRIVATE /w)
endif()

file(GLOB LUA51_SOURCES ${lua51_SOURCE_DIR}/src/*.c)
list(FILTER LUA51_SOURCES EXCLUDE REGEX ".*/(lua|luac|print)\\.c$")

add_library(pt_thirdparty STATIC
  ${volk_SOURCE_DIR}/volk.c
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${LUA51_SOURCES})
target_include_directories(pt_thirdparty PUBLIC
  ${volk_SOURCE_DIR}
  ${vma_SOURCE_DIR}/include
  ${glm_SOURCE_DIR}
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${lua51_SOURCE_DIR}/src
  ${stb_SOURCE_DIR})
target_compile_definitions(pt_thirdparty PUBLIC VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_VOLK)
if(WIN32)
  target_compile_definitions(pt_thirdparty PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
target_link_libraries(pt_thirdparty PUBLIC Vulkan::Headers SDL3::SDL3-static zlibstatic pt_bc7enc)
if(NOT WIN32)
  # Unicode text shaping for the added languages (src/engine/ui/unicode_font_harfbuzz.cpp); Windows uses Uniscribe instead
  FetchContent_Declare(harfbuzz GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git GIT_TAG 14.6.0 GIT_SHALLOW TRUE)
  FetchContent_Populate(harfbuzz)
  add_library(pt_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
  target_include_directories(pt_harfbuzz PUBLIC ${harfbuzz_SOURCE_DIR}/src)
  target_compile_options(pt_harfbuzz PRIVATE -w -fno-exceptions -fno-rtti)
  target_link_libraries(pt_thirdparty PUBLIC pt_harfbuzz)
endif()
if(MSVC)
  target_compile_options(pt_thirdparty PRIVATE /w)
endif()

FetchContent_Declare(ogg GIT_REPOSITORY https://github.com/xiph/ogg.git GIT_TAG v1.3.5 GIT_SHALLOW TRUE)
FetchContent_Declare(vorbis GIT_REPOSITORY https://github.com/xiph/vorbis.git GIT_TAG v1.3.7 GIT_SHALLOW TRUE)
FetchContent_Populate(ogg)
FetchContent_Populate(vorbis)

set(PT_WWISE_CODEBOOKS_DIR ${CMAKE_BINARY_DIR}/_deps/ww2ogg)
set(PT_WWISE_CODEBOOKS_BIN ${PT_WWISE_CODEBOOKS_DIR}/packed_codebooks_aoTuV_603.bin)
set(PT_WWISE_CODEBOOKS_C ${PT_WWISE_CODEBOOKS_DIR}/wwise_codebooks.c)
if(NOT EXISTS ${PT_WWISE_CODEBOOKS_BIN})
  file(DOWNLOAD https://raw.githubusercontent.com/hcs64/ww2ogg/master/packed_codebooks_aoTuV_603.bin ${PT_WWISE_CODEBOOKS_BIN}
    EXPECTED_HASH SHA256=00a93eab267d281401b1efd54e888a2e183299b9e6c446c48d09f701a89d9d27 STATUS pt_codebooks_status)
  list(GET pt_codebooks_status 0 pt_codebooks_code)
  if(NOT pt_codebooks_code EQUAL 0)
    file(REMOVE ${PT_WWISE_CODEBOOKS_BIN})
    message(FATAL_ERROR "cannot download the Wwise Vorbis codebook library: ${pt_codebooks_status}")
  endif()
endif()
if(NOT EXISTS ${PT_WWISE_CODEBOOKS_C} OR ${PT_WWISE_CODEBOOKS_BIN} IS_NEWER_THAN ${PT_WWISE_CODEBOOKS_C})
  file(READ ${PT_WWISE_CODEBOOKS_BIN} pt_codebooks_hex HEX)
  string(LENGTH "${pt_codebooks_hex}" pt_codebooks_hex_length)
  math(EXPR pt_codebooks_size "${pt_codebooks_hex_length} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," pt_codebooks_bytes "${pt_codebooks_hex}")
  file(WRITE ${PT_WWISE_CODEBOOKS_C}
    "#include <stddef.h>\nconst unsigned char pt_wwise_codebooks[] = {${pt_codebooks_bytes}};\nconst size_t pt_wwise_codebooks_size = ${pt_codebooks_size};\n")
endif()

set(PT_VORBIS_SOURCES)
foreach(name mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook
    sharedbook lookup bitrate)
  list(APPEND PT_VORBIS_SOURCES ${vorbis_SOURCE_DIR}/lib/${name}.c)
endforeach()
add_library(pt_vorbis STATIC ${ogg_SOURCE_DIR}/src/bitwise.c ${ogg_SOURCE_DIR}/src/framing.c ${PT_VORBIS_SOURCES} ${PT_WWISE_CODEBOOKS_C})
target_include_directories(pt_vorbis PUBLIC ${ogg_SOURCE_DIR}/include ${vorbis_SOURCE_DIR}/include PRIVATE ${vorbis_SOURCE_DIR}/lib)
if(NOT WIN32)
  # ogg's own build generates this header outside Windows; the sources are compiled directly here
  file(WRITE "${CMAKE_BINARY_DIR}/generated/ogg-config/ogg/config_types.h"
    "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
  target_include_directories(pt_vorbis PUBLIC "${CMAKE_BINARY_DIR}/generated/ogg-config")
endif()
if(MSVC)
  target_compile_options(pt_vorbis PRIVATE /w)
endif()
target_link_libraries(pt_thirdparty PUBLIC pt_vorbis)
