cmake_minimum_required(VERSION 3.28)
project(pt_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # one binary for many distributions: the C++ runtime goes into the executable, only glibc is taken from the system
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -static-libstdc++ -static-libgcc")
  # and into the voice recognizer's libwhisper.so and libggml*.so (cmake/Dependencies.cmake)
  # with their copies of the C++ runtime kept private (--exclude-libs): exported, the copies in libggml.so and libwhisper.so
  # bound to each other and to the system's libstdc++ (which Mesa loads into the game), and the speech model's std::regex
  # crashed in a facet of the wrong copy on SteamOS; -Bsymbolic binds the template code each library has to its own copy too
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-Bsymbolic")
  string(APPEND CMAKE_MODULE_LINKER_FLAGS " -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-Bsymbolic")
endif()
include(cmake/Dependencies.cmake)

file(GLOB_RECURSE PT_ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/*.cpp src/engine/*.h)
file(GLOB_RECURSE PT_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp src/game/*.h src/decomp/*.cpp src/decomp/*.h)

# The release version and the release manifest the game and the installer check for a newer version (docs/updates.md).
# The manifest is latest.json attached to the latest GitHub release (tools/ci/release.py writes it); "releases/latest/download"
# redirects to the newest release's asset, so the address never changes.
# This build's version: raised here for each release. tools/ci/release.py --version sets it through the environment at build
# time (cmake/version.cmake); PT_VERSION_OVERRIDE sets it for one build folder.
set(PT_VERSION_OVERRIDE "" CACHE STRING "Version for this build folder instead of the one in CMakeLists.txt")
set(PT_VERSION "1.0.0")
if(PT_VERSION_OVERRIDE)
  set(PT_VERSION "${PT_VERSION_OVERRIDE}")
endif()
# not a cache variable: an old build folder must not keep a stale address (PT_UPDATE_MANIFEST_URL_OVERRIDE sets one for a build folder)
set(PT_UPDATE_MANIFEST_URL_OVERRIDE "" CACHE STRING "Release manifest for the update check, instead of the GitHub release")
set(PT_UPDATE_MANIFEST_URL "https://github.com/LoreanXavier/pt-pc/releases/latest/download/latest.json")
if(PT_UPDATE_MANIFEST_URL_OVERRIDE)
  set(PT_UPDATE_MANIFEST_URL "${PT_UPDATE_MANIFEST_URL_OVERRIDE}")
endif()
add_custom_target(pt_version
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${CMAKE_BINARY_DIR}/generated/pt_version.h" "-DPT_VERSION=${PT_VERSION}"
          "-DPT_UPDATE_MANIFEST_URL=${PT_UPDATE_MANIFEST_URL}" -P "${CMAKE_SOURCE_DIR}/cmake/version.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/pt_version.h")

add_library(pt_engine STATIC ${PT_ENGINE_SOURCES} ${PT_GAME_SOURCES})
# Game+ (docs/gameplay.md, Game+): the content a finished game unlocks, ON since 1.0 (every release ships it). OFF builds keep
# it inert (the save still stores the finish tag and count, nothing else changes). The first configure takes the default from
# the environment variable PT_GAMEPLUS.
if(DEFINED ENV{PT_GAMEPLUS})
  set(PT_GAMEPLUS_DEFAULT "$ENV{PT_GAMEPLUS}")
else()
  set(PT_GAMEPLUS_DEFAULT ON)
endif()
option(PT_GAMEPLUS "Game+ content after a finished game" ${PT_GAMEPLUS_DEFAULT})
if(PT_GAMEPLUS)
  target_compile_definitions(pt_engine PUBLIC PT_GAMEPLUS=1)
else()
  target_compile_definitions(pt_engine PUBLIC PT_GAMEPLUS=0)
endif()
target_include_directories(pt_engine PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(pt_engine pt_version)
target_include_directories(pt_engine PUBLIC src)
target_compile_definitions(pt_engine PUBLIC VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
target_link_libraries(pt_engine PUBLIC pt_thirdparty SDL3::SDL3-static)
if(WIN32)
  # dbghelp: the crash report (src/engine/core/crash_report.cpp) writes minidumps from pt_engine, so every test links it
  target_link_libraries(pt_engine PUBLIC gdi32 usp10 winhttp dbghelp ws2_32)
else()
  # Linux: the platform layer loads libcurl at run time (src/engine/platform/http.cpp); fonts are shaped with HarfBuzz
  target_link_libraries(pt_engine PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(MSVC)
  target_compile_options(pt_engine PRIVATE /W3 /utf-8 /EHsc)
endif()

add_executable(pt WIN32 src/main.cpp)
target_link_libraries(pt PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt PRIVATE dbghelp ole32 uuid)
endif()
# PT_BUILD_ID: the commit the executable is built from (plus "-dirty" for local changes), in the crash dump names and pt.log
add_custom_target(pt_build_id
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${CMAKE_BINARY_DIR}/generated/build_id.h" "-DSOURCE=${CMAKE_SOURCE_DIR}" -P "${CMAKE_SOURCE_DIR}/cmake/build_id.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/build_id.h")
target_include_directories(pt PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(pt pt_build_id)
# the release 1.0 game: the loop browser stays locked until the game was finished once, its entries open as their loops are reached
# (PT_RELEASE_LOCKS, docs/gameplay.md loop browser); pt (developer) keeps every entry open. tools/ci/release.py
# builds it and tools/package.py --exe ships it as pt.exe
add_executable(pt_release WIN32 EXCLUDE_FROM_ALL src/main.cpp)
target_compile_definitions(pt_release PRIVATE PT_RELEASE_LOCKS=1)
target_include_directories(pt_release PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(pt_release pt_build_id)
target_link_libraries(pt_release PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt_release PRIVATE dbghelp ole32 uuid)
endif()
add_executable(pt_gouge_input_test tests/gouge_input_test.cpp)
target_link_libraries(pt_gouge_input_test PRIVATE pt_engine)
set(PT_SETUP_PAYLOAD "${CMAKE_SOURCE_DIR}/dump/installer-20261002/native/payload.bin" CACHE FILEPATH "Installer payload")
# the setup's product strings (installer/Native/setup.rc.in); the setup checks its own integrity stamp (docs/installer.md)
set(PT_SETUP_PRODUCT "P.T. PC Port")
set(PT_SETUP_DESCRIPTION "P.T. PC Port Setup")
# the version resource: PT_VERSION's numbers (the environment's PT_VERSION, as cmake/version.cmake takes it, wins)
set(PT_SETUP_VERSION "${PT_VERSION}")
if(DEFINED ENV{PT_VERSION} AND NOT "$ENV{PT_VERSION}" STREQUAL "")
  set(PT_SETUP_VERSION "$ENV{PT_VERSION}")
endif()
string(REGEX REPLACE "-.*$" "" PT_SETUP_VERSION_NUMBERS "${PT_SETUP_VERSION}")
string(REPLACE "." ";" PT_SETUP_VERSION_LIST "${PT_SETUP_VERSION_NUMBERS}")
list(APPEND PT_SETUP_VERSION_LIST 0 0 0 0)
list(SUBLIST PT_SETUP_VERSION_LIST 0 4 PT_SETUP_VERSION_LIST)
string(REPLACE ";" "," PT_SETUP_VERSION_RC "${PT_SETUP_VERSION_LIST}")
if(WIN32 AND EXISTS "${PT_SETUP_PAYLOAD}")
  enable_language(RC)
  # the application manifest goes in as a resource (Common Controls 6 for the progress bar, docs/installer.md); the
  # linker must not make its own, or the two collide
  set(PT_SETUP_MANIFEST "${CMAKE_SOURCE_DIR}/installer/Native/setup.manifest")
  configure_file(installer/Native/setup.rc.in "${CMAKE_BINARY_DIR}/private-setup.rc" @ONLY)
  set_source_files_properties("${CMAKE_BINARY_DIR}/private-setup.rc" PROPERTIES OBJECT_DEPENDS "${PT_SETUP_PAYLOAD};${PT_SETUP_MANIFEST}")
  add_executable(pt_setup WIN32 EXCLUDE_FROM_ALL installer/Native/setup.cpp src/engine/platform/http.cpp
    src/engine/platform/update_check.cpp "${CMAKE_BINARY_DIR}/private-setup.rc")
  target_include_directories(pt_setup PRIVATE src installer/Native "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(pt_setup pt_version)
  target_compile_definitions(pt_setup PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(pt_setup PRIVATE zlibstatic winhttp comctl32 comdlg32 shell32 ole32 uuid bcrypt)
  target_link_options(pt_setup PRIVATE /MANIFEST:NO)
  if(MSVC)
    target_compile_options(pt_setup PRIVATE /utf-8 /EHsc)
    target_link_options(pt_setup PRIVATE /OPT:REF /OPT:ICF)
  endif()
endif()
if(NOT WIN32)
  # the Linux installer (installer/Native/setup_linux.cpp); tools/linux/attach_payload.py appends the payload after the build
  add_executable(pt_setup_linux EXCLUDE_FROM_ALL installer/Native/setup_linux.cpp src/engine/platform/http.cpp
    src/engine/platform/update_check.cpp src/engine/platform/os.cpp)
  target_include_directories(pt_setup_linux PRIVATE src installer/Native "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(pt_setup_linux pt_version)
  target_link_libraries(pt_setup_linux PRIVATE zlibstatic ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(MSVC)
  target_compile_options(pt PRIVATE /utf-8 /EHsc)
  target_compile_options(pt_release PRIVATE /utf-8 /EHsc)
endif()

include(cmake/Upscalers.cmake)
include(cmake/EnhancedTextures.cmake)
include(cmake/OpenXR.cmake)
list(APPEND PT_UPSCALER_RUNTIME_FILES ${PT_OPENXR_RUNTIME_FILES})
list(APPEND PT_UPSCALER_NOTICES ${PT_OPENXR_NOTICES})
foreach(file ${PT_UPSCALER_RUNTIME_FILES})
  add_custom_command(TARGET pt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "$<TARGET_FILE_DIR:pt>" VERBATIM)
endforeach()
foreach(entry ${PT_UPSCALER_NOTICES})
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 source)
  list(GET parts 1 name)
  add_custom_command(TARGET pt POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pt>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "$<TARGET_FILE_DIR:pt>/licenses/${name}" VERBATIM)
endforeach()

file(GLOB PT_SHADERS CONFIGURE_DEPENDS shaders/*.vert shaders/*.frag shaders/*.comp)
set(PT_SPIRV_DIR ${CMAKE_BINARY_DIR}/shaders)
set(PT_SPIRV_OUTPUTS)
foreach(shader ${PT_SHADERS})
  get_filename_component(name ${shader} NAME)
  set(out ${PT_SPIRV_DIR}/${name}.spv)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PT_SPIRV_DIR}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -MD -MF ${out}.d ${shader} -o ${out}
    DEPENDS ${shader} DEPFILE ${out}.d VERBATIM)
  list(APPEND PT_SPIRV_OUTPUTS ${out})
endforeach()
add_custom_target(pt_shaders ALL DEPENDS ${PT_SPIRV_OUTPUTS})
add_dependencies(pt pt_shaders)
target_compile_definitions(pt_engine PUBLIC PT_SHADER_DIR="${PT_SPIRV_DIR}" PT_VOICE_MODEL_DIR="${CMAKE_BINARY_DIR}/voice")
target_include_directories(pt_engine PRIVATE ${PT_WHISPER_INCLUDE_DIRS})
# whisper.cpp, ggml and its CPU variants in voice/ for every game target (tools/package.py ships them with any of these)
foreach(game pt pt_release)
  add_dependencies(${game} pt_voice_runtime)
endforeach()

add_executable(pt_tests tests/test_strcode.cpp)
target_link_libraries(pt_tests PRIVATE pt_engine)

add_executable(pt_voice_match_test tests/voice_match_test.cpp)
target_link_libraries(pt_voice_match_test PRIVATE pt_engine)

add_executable(pt_vfx_test tests/vfx_test.cpp)
target_link_libraries(pt_vfx_test PRIVATE pt_engine)

add_executable(pt_collision_test tests/collision_test.cpp)
add_executable(pt_localization_test tests/localization_test.cpp)
target_link_libraries(pt_localization_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_localization_test PRIVATE /utf-8)
endif()
target_link_libraries(pt_collision_test PRIVATE pt_engine)

add_executable(pt_texture_cache_test tests/texture_cache_test.cpp)
target_link_libraries(pt_texture_cache_test PRIVATE pt_engine)

add_executable(pt_texture_job_test tests/texture_job_test.cpp)
target_link_libraries(pt_texture_job_test PRIVATE pt_engine)

set(PT_TEXTURE_DESCRIPTOR_SHADER "${CMAKE_BINARY_DIR}/tests/texture_descriptor.comp.spv")
add_custom_command(OUTPUT "${PT_TEXTURE_DESCRIPTOR_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O "${CMAKE_SOURCE_DIR}/tests/texture_descriptor.comp" -o "${PT_TEXTURE_DESCRIPTOR_SHADER}"
  DEPENDS tests/texture_descriptor.comp VERBATIM)
add_custom_target(pt_texture_descriptor_shader DEPENDS "${PT_TEXTURE_DESCRIPTOR_SHADER}")
add_executable(pt_texture_descriptor_test tests/texture_descriptor_test.cpp)
target_link_libraries(pt_texture_descriptor_test PRIVATE pt_engine)
target_compile_definitions(pt_texture_descriptor_test PRIVATE PT_TEXTURE_DESCRIPTOR_SHADER="${PT_TEXTURE_DESCRIPTOR_SHADER}")
add_dependencies(pt_texture_descriptor_test pt_texture_descriptor_shader)

set(PT_REFLECTION_MIX_SHADER "${CMAKE_BINARY_DIR}/tests/reflection_mix_test.comp.spv")
add_custom_command(OUTPUT "${PT_REFLECTION_MIX_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -I "${CMAKE_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/tests/reflection_mix_test.comp" -o "${PT_REFLECTION_MIX_SHADER}"
  DEPENDS tests/reflection_mix_test.comp shaders/reflection_mix.glsl shaders/mirror_temporal.glsl shaders/flashlight_reactivity.glsl VERBATIM)
add_custom_target(pt_reflection_mix_shader DEPENDS "${PT_REFLECTION_MIX_SHADER}")
add_executable(pt_reflection_mix_test tests/reflection_mix_test.cpp)
target_link_libraries(pt_reflection_mix_test PRIVATE pt_engine)
target_compile_definitions(pt_reflection_mix_test PRIVATE PT_REFLECTION_MIX_SHADER="${PT_REFLECTION_MIX_SHADER}")
add_dependencies(pt_reflection_mix_test pt_reflection_mix_shader)


add_executable(pt_dualsense_probe EXCLUDE_FROM_ALL tests/dualsense_hid_probe.cpp)
target_link_libraries(pt_dualsense_probe PRIVATE pt_engine)

add_executable(pt_audio_test tests/audio_test.cpp)
target_link_libraries(pt_audio_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_audio_test PRIVATE /utf-8 /EHsc)
endif()

add_executable(pt_graphics_preset_test tests/graphics_preset_test.cpp)
target_link_libraries(pt_graphics_preset_test PRIVATE pt_engine)

# the full screen blur's pulse start per game frame (0x9231D0), src/game/blur_sway.h
add_executable(pt_blur_sway_test tests/blur_sway_test.cpp)
target_link_libraries(pt_blur_sway_test PRIVATE pt_engine)

add_executable(pt_settings_roundtrip_test tests/settings_roundtrip_test.cpp)
target_link_libraries(pt_settings_roundtrip_test PRIVATE pt_engine)

# the published body yaw that PlayerInputDir (0x915850) reads
add_executable(pt_player_publish_test tests/player_publish_test.cpp)
target_link_libraries(pt_player_publish_test PRIVATE pt_engine)

# the hair highlight's dominant light direction over time (0x8D12A0)
add_executable(pt_dominant_light_test tests/dominant_light_test.cpp)
target_link_libraries(pt_dominant_light_test PRIVATE pt_engine)

# the VR mode's view mapping (src/engine/xr/xr_view.h, docs/vr.md)
add_executable(pt_vr_view_test tests/vr_view_test.cpp)
target_link_libraries(pt_vr_view_test PRIVATE pt_engine)

# the hardware check of FSR 3 frame generation (src/engine/render/upscale/upscale.h, docs/upscaling.md)
add_executable(pt_upscaler_gpu_test tests/upscaler_gpu_test.cpp)
target_link_libraries(pt_upscaler_gpu_test PRIVATE pt_engine)

add_executable(pt_peephole_look_test tests/peephole_look_test.cpp)
target_link_libraries(pt_peephole_look_test PRIVATE pt_engine)

# the occluder stage of the original's light culling (0xD2A100, 0xD2AC10), src/engine/render/light_cull.cpp
add_executable(pt_light_cull_test tests/light_cull_test.cpp)
target_link_libraries(pt_light_cull_test PRIVATE pt_engine)

# the option defaults from the system language (0x91D7B0)
add_executable(pt_language_default_test tests/language_default_test.cpp)
target_link_libraries(pt_language_default_test PRIVATE pt_engine)

# the boot load's outcomes and the failed writes behind the save dialogs (0x947C60, 0x9476D0)
add_executable(pt_save_status_test tests/save_status_test.cpp)
target_link_libraries(pt_save_status_test PRIVATE pt_engine)

add_executable(pt_save_reset_test tests/save_reset_test.cpp)
target_link_libraries(pt_save_reset_test PRIVATE pt_engine)

add_executable(pt_multilingual_test tests/multilingual_test.cpp)
target_link_libraries(pt_multilingual_test PRIVATE pt_engine)
if(MSVC)
 target_compile_options(pt_multilingual_test PRIVATE /utf-8)
endif()

target_compile_definitions(pt_engine PUBLIC PT_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET pt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:pt>/fonts")
# the bundled fonts' SIL OFL notices also go to licenses/, beside the SDK notices
file(GLOB PT_FONT_LICENSES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/fonts/OFL-*.txt")
foreach(license ${PT_FONT_LICENSES})
  get_filename_component(license_name "${license}" NAME)
  add_custom_command(TARGET pt POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pt>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "$<TARGET_FILE_DIR:pt>/licenses/${license_name}" VERBATIM)
endforeach()

add_executable(pt_mouse_render_test tests/mouse_render_test.cpp)
target_link_libraries(pt_mouse_render_test PRIVATE pt_engine)

add_executable(pt_skin_render_test tests/skin_render_test.cpp)
target_link_libraries(pt_skin_render_test PRIVATE pt_engine)

# the mod override index, mod.json and the mod script sandbox (docs/modding.md); no game files needed
# the platform layer (src/engine/platform: os, http, update check) on Windows and Linux, docs/linux.md
add_executable(pt_platform_test tests/platform_test.cpp)
target_link_libraries(pt_platform_test PRIVATE pt_engine)

add_executable(pt_mods_test tests/mods_test.cpp)
target_link_libraries(pt_mods_test PRIVATE pt_engine)

