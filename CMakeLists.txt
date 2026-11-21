cmake_minimum_required(VERSION 3.28)
# macOS on Apple silicon (docs/macos.md): <format> needs 13.3, every Apple silicon Mac runs 14. Left empty (a build folder
# configured before this line) the build would run only on the macOS it was built on; other hosts do not get the cache entry
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Oldest macOS the build runs on" FORCE)
endif()
# OBJCXX on macOS for the Apple MetalFX upscaler (cmake/MacOS.cmake, metalfx_backend.mm); the language is set here so its
# variables are initialized, as enable_language after project does not
if(CMAKE_HOST_APPLE OR APPLE)
  project(pt_port LANGUAGES C CXX OBJCXX)
else()
  project(pt_port LANGUAGES C CXX)
endif()

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
# the D3D12 renderer backend is its own library, Windows only (cmake/D3D12Backend.cmake)
list(FILTER PT_ENGINE_SOURCES EXCLUDE REGEX "src/engine/render/d3d12/backend/")

# The release version and metadata endpoint shared by the game and installers (docs/updates.md).
# GitHub supplies release metadata and platform asset URLs; no separate JSON release asset is needed.
# This build's version: raised here for each release. tools/ci/release.py --version sets it through the environment at build
# time (cmake/version.cmake); PT_VERSION_OVERRIDE sets it for one build folder.
set(PT_VERSION_OVERRIDE "" CACHE STRING "Version for this build folder instead of the one in CMakeLists.txt")
set(PT_VERSION "1.1.0")
if(PT_VERSION_OVERRIDE)
  set(PT_VERSION "${PT_VERSION_OVERRIDE}")
endif()
# not a cache variable: an old build folder must not keep a stale address (PT_UPDATE_MANIFEST_URL_OVERRIDE sets one for a build folder)
set(PT_UPDATE_MANIFEST_URL_OVERRIDE "" CACHE STRING "Update metadata endpoint, instead of the GitHub Releases API")
set(PT_UPDATE_MANIFEST_URL "https://api.github.com/repos/LoreanXavier/pt-pc/releases/latest")
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
# the first start without game files (src/game/first_run.cpp) uses the setup's own package checks (installer/Native/setup_core.h)
target_include_directories(pt_engine PRIVATE installer/Native)
add_dependencies(pt_engine pt_version)
target_include_directories(pt_engine PUBLIC src)
target_compile_definitions(pt_engine PUBLIC VK_NO_PROTOTYPES NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
target_link_libraries(pt_engine PUBLIC pt_thirdparty SDL3::SDL3-static pt_sbc)
if(WIN32)
  # dbghelp: the crash report (src/engine/core/crash_report.cpp) writes minidumps from pt_engine, so every test links it
  target_link_libraries(pt_engine PUBLIC gdi32 usp10 winhttp dbghelp ws2_32 bcrypt)
else()
  # Linux: the platform layer loads libcurl at run time (src/engine/platform/http.cpp); fonts are shaped with HarfBuzz
  target_link_libraries(pt_engine PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(MSVC)
  target_compile_options(pt_engine PRIVATE /W3 /utf-8 /EHsc)
endif()

# the product strings of the version resources (assets/pt.rc.in, installer/Native/setup.rc.in, updater.rc.in); the setup checks
# its own integrity stamp (docs/installer.md). A program without one is a mark against it with the virus scanners' models
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
if(WIN32)
  enable_language(RC)
  configure_file(assets/pt.rc.in "${CMAKE_BINARY_DIR}/pt.rc" @ONLY)
  # the icon and the manifest are read by the resource compiler, which the build does not track: a new pt.ico left the old one
  # in pt.exe. The manifest (UTF-8 ANSI code page, long paths) goes in as a resource; the linker must not make its own.
  set_source_files_properties("${CMAKE_BINARY_DIR}/pt.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/pt.ico;${CMAKE_SOURCE_DIR}/assets/pt.manifest")
endif()
add_executable(pt WIN32 src/main.cpp)
if(WIN32)
  target_sources(pt PRIVATE "${CMAKE_BINARY_DIR}/pt.rc")
  target_link_options(pt PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(pt PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt PRIVATE dbghelp ole32 uuid)
  # DLSS 5 on RTX 20, 30 and 40 (docs/upscaling.md): the community route puts a ReShade dxgi.dll next to pt.exe. dxgi.dll is
  # delay loaded so the game chooses at startup which one enters the process (nr_unlock::LoadDxgiForRoute) - ReShade only while
  # neural rendering is on, Windows' own otherwise - instead of the loader binding the proxy before main runs.
  target_link_options(pt PRIVATE /DELAYLOAD:dxgi.dll)
  target_link_libraries(pt PRIVATE delayimp)
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
if(WIN32)
  target_sources(pt_release PRIVATE "${CMAKE_BINARY_DIR}/pt.rc")
  target_link_options(pt_release PRIVATE /MANIFEST:NO)
endif()
target_compile_definitions(pt_release PRIVATE PT_RELEASE_LOCKS=1)
target_include_directories(pt_release PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(pt_release pt_build_id)
target_link_libraries(pt_release PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt_release PRIVATE dbghelp ole32 uuid)
  target_link_options(pt_release PRIVATE /DELAYLOAD:dxgi.dll)
  target_link_libraries(pt_release PRIVATE delayimp)
endif()
add_executable(pt_gouge_input_test tests/gouge_input_test.cpp)
target_link_libraries(pt_gouge_input_test PRIVATE pt_engine)
add_executable(pt_outro_input_test tests/outro_input_test.cpp)
target_link_libraries(pt_outro_input_test PRIVATE pt_engine)
set(PT_SETUP_PAYLOAD "${CMAKE_SOURCE_DIR}/dump/installer-20261002/native/payload.bin" CACHE FILEPATH "Installer payload")
if(WIN32 AND EXISTS "${PT_SETUP_PAYLOAD}")
  enable_language(RC)
  # the application manifest goes in as a resource (Common Controls 6 for the progress bar, docs/installer.md); the
  # linker must not make its own, or the two collide
  set(PT_SETUP_MANIFEST "${CMAKE_SOURCE_DIR}/installer/Native/setup.manifest")
  configure_file(installer/Native/setup.rc.in "${CMAKE_BINARY_DIR}/private-setup.rc" @ONLY)
  set_source_files_properties("${CMAKE_BINARY_DIR}/private-setup.rc" PROPERTIES OBJECT_DEPENDS "${PT_SETUP_PAYLOAD};${PT_SETUP_MANIFEST};${CMAKE_SOURCE_DIR}/assets/pt.ico")
  add_executable(pt_setup WIN32 EXCLUDE_FROM_ALL installer/Native/setup.cpp src/engine/platform/http.cpp
    src/engine/platform/update_check.cpp src/engine/platform/ed25519.cpp src/engine/platform/os.cpp src/engine/core/log.cpp "${CMAKE_BINARY_DIR}/private-setup.rc")
  target_include_directories(pt_setup PRIVATE src installer/Native "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(pt_setup pt_version)
  target_compile_definitions(pt_setup PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(pt_setup PRIVATE zlibstatic winhttp comctl32 comdlg32 shell32 ole32 oleaut32 uuid bcrypt gdiplus advapi32)
  target_link_options(pt_setup PRIVATE /MANIFEST:NO)
  if(MSVC)
    target_compile_options(pt_setup PRIVATE /utf-8 /EHsc)
    target_link_options(pt_setup PRIVATE /OPT:REF /OPT:ICF)
  endif()
endif()
if(WIN32)
  # the in-game update's updater (docs/updates.md): the setup's program without its payload, shipped next to the game as
  # pt-updater.exe (tools/package.py); built with the game so every build folder has it
  enable_language(RC)
  set(PT_SETUP_MANIFEST "${CMAKE_SOURCE_DIR}/installer/Native/setup.manifest")
  configure_file(installer/Native/updater.rc.in "${CMAKE_BINARY_DIR}/private-updater.rc" @ONLY)
  set_source_files_properties("${CMAKE_BINARY_DIR}/private-updater.rc" PROPERTIES OBJECT_DEPENDS "${PT_SETUP_MANIFEST};${CMAKE_SOURCE_DIR}/assets/pt.ico")
  add_executable(pt_updater WIN32 installer/Native/setup.cpp src/engine/platform/http.cpp src/engine/platform/update_check.cpp src/engine/core/log.cpp
    src/engine/platform/ed25519.cpp src/engine/platform/os.cpp "${CMAKE_BINARY_DIR}/private-updater.rc")
  set_target_properties(pt_updater PROPERTIES OUTPUT_NAME "pt-updater")
  target_include_directories(pt_updater PRIVATE src installer/Native "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(pt_updater pt_version)
  target_compile_definitions(pt_updater PRIVATE PT_UPDATER_ONLY NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(pt_updater PRIVATE zlibstatic winhttp comctl32 comdlg32 shell32 ole32 oleaut32 uuid bcrypt gdiplus advapi32)
  target_link_options(pt_updater PRIVATE /MANIFEST:NO)
  if(MSVC)
    target_compile_options(pt_updater PRIVATE /utf-8 /EHsc)
    target_link_options(pt_updater PRIVATE /OPT:REF /OPT:ICF)
  endif()
  add_dependencies(pt pt_updater)
endif()
if(NOT WIN32)
  # the Linux installer (installer/Native/setup_linux.cpp); tools/linux/attach_payload.py appends the payload after the build
  add_executable(pt_setup_linux EXCLUDE_FROM_ALL installer/Native/setup_linux.cpp src/engine/platform/http.cpp
    src/engine/platform/update_check.cpp src/engine/platform/ed25519.cpp src/engine/platform/os.cpp src/engine/core/log.cpp)
  target_include_directories(pt_setup_linux PRIVATE src installer/Native "${CMAKE_BINARY_DIR}/generated")
  add_dependencies(pt_setup_linux pt_version)
  target_link_libraries(pt_setup_linux PRIVATE zlibstatic ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(MSVC)
  target_compile_options(pt PRIVATE /utf-8 /EHsc)
  target_compile_options(pt_release PRIVATE /utf-8 /EHsc)
endif()

include(cmake/D3D12.cmake)
include(cmake/Upscalers.cmake)
include(cmake/OpenXR.cmake)
if(APPLE)
  include(cmake/MacOS.cmake)
endif()
list(APPEND PT_UPSCALER_RUNTIME_FILES ${PT_OPENXR_RUNTIME_FILES})
list(APPEND PT_UPSCALER_NOTICES ${PT_OPENXR_NOTICES})
list(APPEND PT_UPSCALER_NOTICES "${google_sbc_SOURCE_DIR}/LICENSE|google-libsbc-Apache-2.0.txt")
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
set(PT_GLSLC_DEFINES)
if(APPLE)
  # MoltenVK: no comparison sampler on the images[] array (shaders/lighting.glsl ShadowTap, docs/macos.md)
  list(APPEND PT_GLSLC_DEFINES -DPT_SHADOW_GATHER)
endif()
foreach(shader ${PT_SHADERS})
  get_filename_component(name ${shader} NAME)
  set(out ${PT_SPIRV_DIR}/${name}.spv)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PT_SPIRV_DIR}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O ${PT_GLSLC_DEFINES} -MD -MF ${out}.d ${shader} -o ${out}
    DEPENDS ${shader} DEPFILE ${out}.d VERBATIM)
  list(APPEND PT_SPIRV_OUTPUTS ${out})
endforeach()
add_custom_target(pt_shaders ALL DEPENDS ${PT_SPIRV_OUTPUTS})
add_dependencies(pt pt_shaders)
if(WIN32)
  # the same shaders as DXIL for the D3D12 backend (cmake/D3D12Shaders.cmake); the Vulkan SPIR-V above is unchanged; the D3D12 backend loads these
  option(PT_D3D12_SHADERS "Translate the shaders to DXIL (GLSL -> SPIR-V -> SPIRV-Cross -> DXC) for the D3D12 backend" ON)
  if(PT_D3D12_SHADERS)
    include(cmake/D3D12Shaders.cmake)
  endif()
  # the D3D12 renderer backend and its test (docs/rhi.md, stage b); the game does not link it yet
  option(PT_D3D12_BACKEND "Build the Direct3D 12 renderer backend library and its test" ON)
  if(PT_D3D12_BACKEND)
    include(cmake/D3D12Backend.cmake)
  endif()
endif()
target_compile_definitions(pt_engine PUBLIC PT_SHADER_DIR="${PT_SPIRV_DIR}" PT_VOICE_MODEL_DIR="${CMAKE_BINARY_DIR}/voice")
target_include_directories(pt_engine PRIVATE ${PT_WHISPER_INCLUDE_DIRS} "${whisper_SOURCE_DIR}/ggml/src")
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
add_executable(pt_parallel_for_test tests/parallel_for_test.cpp)
target_link_libraries(pt_parallel_for_test PRIVATE pt_engine)
add_executable(pt_vk_address_log_test tests/vk_address_log_test.cpp)
target_link_libraries(pt_vk_address_log_test PRIVATE pt_engine)
add_executable(pt_localization_test tests/localization_test.cpp)
target_link_libraries(pt_localization_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_localization_test PRIVATE /utf-8)
endif()
target_link_libraries(pt_collision_test PRIVATE pt_engine)

set(PT_TEXTURE_DESCRIPTOR_SHADER "${CMAKE_BINARY_DIR}/tests/texture_descriptor.comp.spv")
add_custom_command(OUTPUT "${PT_TEXTURE_DESCRIPTOR_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O "${CMAKE_SOURCE_DIR}/tests/texture_descriptor.comp" -o "${PT_TEXTURE_DESCRIPTOR_SHADER}"
  DEPENDS tests/texture_descriptor.comp VERBATIM)
add_custom_target(pt_texture_descriptor_shader DEPENDS "${PT_TEXTURE_DESCRIPTOR_SHADER}")
add_executable(pt_texture_descriptor_test tests/texture_descriptor_test.cpp)
target_link_libraries(pt_texture_descriptor_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_texture_descriptor_test PRIVATE pt_gpu_d3d12)
endif()
target_compile_definitions(pt_texture_descriptor_test PRIVATE PT_TEXTURE_DESCRIPTOR_SHADER="${PT_TEXTURE_DESCRIPTOR_SHADER}")
add_dependencies(pt_texture_descriptor_test pt_texture_descriptor_shader)

set(PT_REFLECTION_MIX_SHADER "${CMAKE_BINARY_DIR}/tests/reflection_mix_test.comp.spv")
add_custom_command(OUTPUT "${PT_REFLECTION_MIX_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -I "${CMAKE_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/tests/reflection_mix_test.comp" -o "${PT_REFLECTION_MIX_SHADER}"
  DEPENDS tests/reflection_mix_test.comp shaders/common.glsl shaders/reflection_blend.glsl shaders/reflection_mix.glsl shaders/mirror_temporal.glsl shaders/flashlight_reactivity.glsl VERBATIM)
add_custom_target(pt_reflection_mix_shader DEPENDS "${PT_REFLECTION_MIX_SHADER}")
add_executable(pt_reflection_mix_test tests/reflection_mix_test.cpp)
target_link_libraries(pt_reflection_mix_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_reflection_mix_test PRIVATE pt_gpu_d3d12)
endif()
target_compile_definitions(pt_reflection_mix_test PRIVATE PT_REFLECTION_MIX_SHADER="${PT_REFLECTION_MIX_SHADER}")
add_dependencies(pt_reflection_mix_test pt_reflection_mix_shader)

set(PT_NATIVE_PROJECTION_SHADER "${CMAKE_BINARY_DIR}/tests/rt_native_projection_test.comp.spv")
add_custom_command(OUTPUT "${PT_NATIVE_PROJECTION_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -I "${CMAKE_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/tests/rt_native_projection_test.comp" -o "${PT_NATIVE_PROJECTION_SHADER}"
  DEPENDS tests/rt_native_projection_test.comp shaders/pt_native_projection.glsl VERBATIM)
add_custom_target(pt_native_projection_shader DEPENDS "${PT_NATIVE_PROJECTION_SHADER}")
add_executable(pt_native_projection_test tests/rt_native_projection_test.cpp)
target_link_libraries(pt_native_projection_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_native_projection_test PRIVATE pt_gpu_d3d12)
endif()
target_compile_definitions(pt_native_projection_test PRIVATE PT_NATIVE_PROJECTION_SHADER="${PT_NATIVE_PROJECTION_SHADER}")
add_dependencies(pt_native_projection_test pt_native_projection_shader)

set(PT_RT_FITTING_POLICY_SHADER "${CMAKE_BINARY_DIR}/tests/rt_fitting_policy_test.comp.spv")
add_custom_command(OUTPUT "${PT_RT_FITTING_POLICY_SHADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -I "${CMAKE_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/tests/rt_fitting_policy_test.comp" -o "${PT_RT_FITTING_POLICY_SHADER}"
  DEPENDS tests/rt_fitting_policy_test.comp shaders/rt_fitting_policy.glsl VERBATIM)
add_custom_target(pt_rt_fitting_policy_shader DEPENDS "${PT_RT_FITTING_POLICY_SHADER}")
add_executable(pt_rt_fitting_policy_test tests/rt_fitting_policy_test.cpp)
target_link_libraries(pt_rt_fitting_policy_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_rt_fitting_policy_test PRIVATE pt_gpu_d3d12)
endif()
target_compile_definitions(pt_rt_fitting_policy_test PRIVATE PT_RT_FITTING_POLICY_SHADER="${PT_RT_FITTING_POLICY_SHADER}")
add_dependencies(pt_rt_fitting_policy_test pt_rt_fitting_policy_shader)

add_executable(pt_dualsense_probe EXCLUDE_FROM_ALL tests/dualsense_hid_probe.cpp)
target_link_libraries(pt_dualsense_probe PRIVATE pt_engine)

add_executable(pt_audio_channel_layout_test tests/audio_channel_layout_test.cpp)
target_link_libraries(pt_audio_channel_layout_test PRIVATE pt_engine)

add_executable(pt_controller_feedback_test tests/controller_feedback_test.cpp)
target_link_libraries(pt_controller_feedback_test PRIVATE pt_engine)

add_executable(pt_photo_preset_test tests/photo_preset_test.cpp)
target_link_libraries(pt_photo_preset_test PRIVATE pt_engine)

add_executable(pt_photo_panel_test tests/photo_panel_test.cpp)
target_link_libraries(pt_photo_panel_test PRIVATE pt_engine)

add_executable(pt_controller_speaker_route_test tests/controller_speaker_route_test.cpp)
target_link_libraries(pt_controller_speaker_route_test PRIVATE pt_engine)
# the main mix's playback device and the DualSense speaker path with fake endpoints (output_device.h)
add_executable(pt_output_device_test tests/output_device_test.cpp)
target_link_libraries(pt_output_device_test PRIVATE pt_engine)
# every event of the banks rendered offline: only the controller speaker events reach the controller stream (needs the game folder)
add_executable(pt_controller_bus_routing_test tests/controller_bus_routing_test.cpp)
target_link_libraries(pt_controller_bus_routing_test PRIVATE pt_engine)
add_executable(pt_spatial_objects_test tests/spatial_objects_test.cpp)
target_link_libraries(pt_spatial_objects_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_output_device_test PRIVATE /utf-8)
endif()
add_executable(pt_dualshock_audio_test tests/dualshock_audio_test.cpp)
target_link_libraries(pt_dualshock_audio_test PRIVATE pt_engine)

add_executable(pt_hdr_output_test tests/hdr_output_test.cpp)
target_link_libraries(pt_hdr_output_test PRIVATE pt_engine)
add_executable(pt_d3d12_hdr_framegen_test tests/d3d12_hdr_framegen_test.cpp)
target_include_directories(pt_d3d12_hdr_framegen_test PRIVATE src)
add_executable(pt_composite_output_test tests/composite_output_test.cpp)
target_include_directories(pt_composite_output_test PRIVATE ${glm_SOURCE_DIR})

add_executable(pt_audio_test tests/audio_test.cpp)
target_link_libraries(pt_audio_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_audio_test PRIVATE /utf-8 /EHsc)
endif()

add_executable(pt_graphics_preset_test tests/graphics_preset_test.cpp)
target_link_libraries(pt_graphics_preset_test PRIVATE pt_engine)

# implicit Vulkan layer manifests and the environment that keeps a crashing layer out (src/engine/render/vk_layers.h)
add_executable(pt_vk_layers_test tests/vk_layers_test.cpp)
target_link_libraries(pt_vk_layers_test PRIVATE pt_engine)
# the third-party tools in the process: a proxy DLL loaded from the executable's folder, and the tools named by their PDBs
# (src/engine/platform/third_party.h)
add_executable(pt_third_party_test tests/third_party_test.cpp)
target_link_libraries(pt_third_party_test PRIVATE pt_engine)
# the Intel Windows drivers that crash on the first frame and the notice's texts (src/engine/render/intel_vulkan_driver.h)
add_executable(pt_intel_vulkan_driver_test tests/intel_vulkan_driver_test.cpp)
target_link_libraries(pt_intel_vulkan_driver_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_intel_vulkan_driver_test PRIVATE /utf-8)
endif()
if(WIN32)
  # a test implicit layer that faults on vkDestroySwapchainKHR(VK_NULL_HANDLE) like Bandicam's and Mirillis Action's
  # (tools/vk_null_swapchain_layer): its own folder, never packaged; a run selects it with
  # VK_ADD_IMPLICIT_LAYER_PATH=<build>/vk_null_swapchain_layer and ENABLE_PT_NULL_SWAPCHAIN_LAYER=1
  add_library(pt_null_swapchain_layer SHARED EXCLUDE_FROM_ALL tools/vk_null_swapchain_layer/vk_null_swapchain_layer.cpp)
  target_link_libraries(pt_null_swapchain_layer PRIVATE Vulkan::Headers)
  set_target_properties(pt_null_swapchain_layer PROPERTIES OUTPUT_NAME "pt_null_swapchain_layer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vk_null_swapchain_layer" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vk_null_swapchain_layer")
  file(WRITE "${CMAKE_BINARY_DIR}/vk_null_swapchain_layer/pt_null_swapchain_layer.json"
    "{\n  \"file_format_version\": \"1.1.2\",\n  \"layer\": {\n    \"name\": \"VK_LAYER_PT_null_swapchain_test\",\n    \"type\": \"GLOBAL\",\n    \"library_path\": \".\\\\pt_null_swapchain_layer.dll\",\n    \"api_version\": \"1.3.0\",\n    \"implementation_version\": \"1\",\n    \"description\": \"pt-port test: faults on vkDestroySwapchainKHR(VK_NULL_HANDLE)\",\n    \"functions\": { \"vkNegotiateLoaderLayerInterfaceVersion\": \"PtNullSwapchainNegotiate\" },\n    \"disable_environment\": { \"DISABLE_PT_NULL_SWAPCHAIN_LAYER\": \"1\" },\n    \"enable_environment\": { \"ENABLE_PT_NULL_SWAPCHAIN_LAYER\": \"1\" }\n  }\n}\n")
endif()

# the full screen blur's pulse start per game frame (0x9231D0), src/game/blur_sway.h
add_executable(pt_blur_sway_test tests/blur_sway_test.cpp)
target_link_libraries(pt_blur_sway_test PRIVATE pt_engine)

# P.T. game files of every release are found by their content, not their folder's name (src/engine/fs/game_folder.h)
add_executable(pt_game_folder_test tests/game_folder_test.cpp)
target_include_directories(pt_game_folder_test PRIVATE src)
add_executable(pt_unicode_paths_test tests/unicode_paths_test.cpp)
target_link_libraries(pt_unicode_paths_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_unicode_paths_test PRIVATE pt_gpu_d3d12)
endif()
# the installer's ASCII file name rules (setup_core.h) are checked under a Turkish locale too
target_include_directories(pt_unicode_paths_test PRIVATE installer/Native)
if(WIN32)
  target_sources(pt_unicode_paths_test PRIVATE tests/unicode_paths_test.rc)
  set_source_files_properties(tests/unicode_paths_test.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unicode_paths_test.manifest")
  target_link_options(pt_unicode_paths_test PRIVATE /MANIFEST:NO)
  target_link_libraries(pt_unicode_paths_test PRIVATE shell32)
endif()
if(MSVC)
  target_compile_options(pt_unicode_paths_test PRIVATE /utf-8)
endif()

add_executable(pt_user_data_test tests/user_data_test.cpp)
target_link_libraries(pt_user_data_test PRIVATE pt_engine)
if(APPLE)
  add_executable(pt_macos_data_path_test tests/macos_data_path_test.cpp
    src/engine/platform/macos_data_path.cpp src/engine/platform/user_data.cpp src/engine/platform/os.cpp)
  target_include_directories(pt_macos_data_path_test PRIVATE src)
  target_link_libraries(pt_macos_data_path_test PRIVATE "-framework CoreFoundation")
endif()
add_executable(pt_crash_upload_test tests/crash_upload_test.cpp)
target_link_libraries(pt_crash_upload_test PRIVATE pt_engine)

add_executable(pt_settings_roundtrip_test tests/settings_roundtrip_test.cpp)
target_link_libraries(pt_settings_roundtrip_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_settings_roundtrip_test PRIVATE /utf-8)
endif()

# the Rendering API choice: parsing, the Vulkan fallback, pt.ini keys, the texts in 14 languages (engine/platform/rendering_api.h)
add_executable(pt_rendering_api_test tests/rendering_api_test.cpp)
target_link_libraries(pt_rendering_api_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_rendering_api_test PRIVATE /utf-8)
endif()

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
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_upscaler_gpu_test PRIVATE pt_gpu_d3d12)
endif()

if(WIN32)
  add_executable(pt_d3d12_ngx_identity_test tests/d3d12_ngx_identity_test.cpp)
  target_include_directories(pt_d3d12_ngx_identity_test PRIVATE src)
endif()

add_executable(pt_fsr_provider_gate_test tests/fsr_provider_gate_test.cpp src/engine/render/upscale/fsr_provider_gate.cpp)
target_include_directories(pt_fsr_provider_gate_test PRIVATE src)

add_executable(pt_nr_toggle_policy_test tests/nr_toggle_policy_test.cpp)
target_include_directories(pt_nr_toggle_policy_test PRIVATE src)

add_executable(pt_nr_unlock_test tests/nr_unlock_test.cpp)
target_link_libraries(pt_nr_unlock_test PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt_nr_unlock_test PRIVATE winhttp)
endif()

# what the monitor shows frame by frame (Desktop Duplication), for the frame generation checks (tools/fg_screen_check.py)
if(WIN32)
  add_executable(pt_screen_capture tests/screen_capture.cpp)
  target_link_libraries(pt_screen_capture PRIVATE d3d11 dxgi)
  # ReShade's update check kept off the network by a process-scoped WinINet proxy (docs/upscaling.md)
  add_executable(pt_wininet_block_test tests/wininet_block_test.cpp)
  target_link_libraries(pt_wininet_block_test PRIVATE wininet)
endif()

add_executable(pt_fg_unlock_test tests/fg_unlock_test.cpp)
target_link_libraries(pt_fg_unlock_test PRIVATE pt_engine)
if(WIN32)
  target_link_libraries(pt_fg_unlock_test PRIVATE winhttp)
endif()

# the restart queue of the two community routes (src/engine/render/upscale/route_queue.h, tools/route_queue_check.py)
add_executable(pt_route_queue_test tests/route_queue_test.cpp)
target_link_libraries(pt_route_queue_test PRIVATE pt_engine)
if(WIN32)
  # stands in for pt.exe and DLSS5-Autopilot in the pt-updater restart queue test
  add_executable(pt_route_fake_tool tests/route_fake_tool.cpp)
  target_compile_definitions(pt_route_fake_tool PRIVATE UNICODE _UNICODE)
endif()

add_executable(pt_peephole_look_test tests/peephole_look_test.cpp)
target_link_libraries(pt_peephole_look_test PRIVATE pt_engine)

add_executable(pt_sim_rigid_test tests/sim_rigid_test.cpp)
target_link_libraries(pt_sim_rigid_test PRIVATE pt_engine)
target_compile_definitions(pt_sim_rigid_test PRIVATE PT_BABY_SIM_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/baby_sim_ps4.txt")

add_executable(pt_sim_jacket_test tests/sim_jacket_test.cpp)
target_link_libraries(pt_sim_jacket_test PRIVATE pt_engine)
target_compile_definitions(pt_sim_jacket_test PRIVATE PT_JACKET_SIM_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/jacket_sim_ps4.txt")

add_executable(pt_sound_captions_test tests/sound_captions_test.cpp)
target_link_libraries(pt_sound_captions_test PRIVATE pt_engine)
if(MSVC)
  target_compile_options(pt_sound_captions_test PRIVATE /utf-8)
endif()

add_executable(pt_gyro_test tests/gyro_test.cpp)
target_link_libraries(pt_gyro_test PRIVATE pt_engine)
# the start's controllers and audio devices off the main thread, with slow starts simulated (src/engine/platform/device_start.h)
add_executable(pt_device_start_test tests/device_start_test.cpp)
target_link_libraries(pt_device_start_test PRIVATE pt_engine)

# the occluder stage of the original's light culling (0xD2A100, 0xD2AC10), src/engine/render/light_cull.cpp
add_executable(pt_light_cull_test tests/light_cull_test.cpp)
target_link_libraries(pt_light_cull_test PRIVATE pt_engine)

add_executable(pt_point_shadow_projection_test tests/point_shadow_projection_test.cpp)
target_include_directories(pt_point_shadow_projection_test PRIVATE src "${glm_SOURCE_DIR}")

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
# the survival night's guns (the port's own models in glTF with their points in guns.json, made by tools/survival_guns): beside the
# game in survival/, copied on every build so a remade gun reaches the build folder without relinking
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/survival")
  add_custom_target(pt_survival_models ALL COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/survival" "$<TARGET_FILE_DIR:pt>/survival")
  add_dependencies(pt pt_survival_models)
  add_dependencies(pt_release pt_survival_models)
endif()
# The port's data pack beside the game (src/engine/fs/data_pack.h): the bonus scene's files of assets/lisa_date packed by
# tools/make_data_pack.py whenever one of them changes, or a tree without that folder takes the pack it ships (assets/extras.pak).
# No loose copy of the folder goes beside the game; the one older builds left there is removed.
set(PT_DATA_PACK "${CMAKE_BINARY_DIR}/extras.pak")
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/lisa_date/script.txt")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  file(GLOB_RECURSE PT_DATA_PACK_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/lisa_date/*")
  add_custom_command(OUTPUT "${PT_DATA_PACK}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_data_pack.py" --source "${CMAKE_SOURCE_DIR}/assets/lisa_date" --out "${PT_DATA_PACK}"
    COMMAND ${CMAKE_COMMAND} -E touch "${PT_DATA_PACK}"
    DEPENDS ${PT_DATA_PACK_INPUTS} "${CMAKE_SOURCE_DIR}/tools/make_data_pack.py" VERBATIM)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/assets/extras.pak")
  add_custom_command(OUTPUT "${PT_DATA_PACK}" COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/assets/extras.pak" "${PT_DATA_PACK}"
    DEPENDS "${CMAKE_SOURCE_DIR}/assets/extras.pak" VERBATIM)
else()
  message(WARNING "Neither assets/lisa_date nor assets/extras.pak: the game builds without its data pack")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/lisa_date/script.txt" OR EXISTS "${CMAKE_SOURCE_DIR}/assets/extras.pak")
  add_custom_target(pt_data_pack ALL DEPENDS "${PT_DATA_PACK}" COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/lisa_date")
  add_dependencies(pt pt_data_pack)
  add_dependencies(pt_release pt_data_pack)
endif()
# the bundled fonts' SIL OFL notices also go to licenses/, beside the SDK notices
file(GLOB PT_FONT_LICENSES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/fonts/OFL-*.txt")
foreach(license ${PT_FONT_LICENSES})
  get_filename_component(license_name "${license}" NAME)
  add_custom_command(TARGET pt POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pt>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "$<TARGET_FILE_DIR:pt>/licenses/${license_name}" VERBATIM)
endforeach()

add_executable(pt_mouse_render_test tests/mouse_render_test.cpp)
target_link_libraries(pt_mouse_render_test PRIVATE pt_engine)

add_executable(pt_camera_blend_test tests/camera_blend_test.cpp)
target_link_libraries(pt_camera_blend_test PRIVATE pt_engine)
add_executable(pt_rt_record_tint_test tests/rt_record_tint_test.cpp)
target_link_libraries(pt_rt_record_tint_test PRIVATE pt_engine)

# the house's motion blur takes the camera of one original frame ago at any frame rate (src/engine/render/motion_window.h)
add_executable(pt_motion_window_test tests/motion_window_test.cpp)
target_link_libraries(pt_motion_window_test PRIVATE pt_engine)

add_executable(pt_skin_render_test tests/skin_render_test.cpp)
target_link_libraries(pt_skin_render_test PRIVATE pt_engine)

add_executable(pt_rt_record_flags_test tests/rt_record_flags_test.cpp)
target_include_directories(pt_rt_record_flags_test PRIVATE src "${glm_SOURCE_DIR}")

add_executable(pt_foliage_shadow_test tests/pt_foliage_shadow_test.cpp)
target_include_directories(pt_foliage_shadow_test PRIVATE src "${glm_SOURCE_DIR}")
add_executable(pt_skin_shadow_receiver_test tests/rt_skin_shadow_receiver_test.cpp)
target_include_directories(pt_skin_shadow_receiver_test PRIVATE "${glm_SOURCE_DIR}")

# the mod override index, mod.json and the mod script sandbox (docs/modding.md); no game files needed
# the platform layer (src/engine/platform: os, http, update check) on Windows and Linux, docs/linux.md
add_executable(pt_platform_test tests/platform_test.cpp)
target_link_libraries(pt_platform_test PRIVATE pt_engine)

add_executable(pt_mods_test tests/mods_test.cpp)
target_link_libraries(pt_mods_test PRIVATE pt_engine)


add_executable(pt_lisa_date_test tests/lisa_date_test.cpp)
target_link_libraries(pt_lisa_date_test PRIVATE pt_engine)
# the test reads the source files themselves and compares the data pack with them
target_compile_definitions(pt_lisa_date_test PRIVATE PT_LISA_DATE_DIR="${CMAKE_SOURCE_DIR}/assets/lisa_date" PT_DATA_PACK_FILE="${PT_DATA_PACK}")

# the prompts' keycaps follow the keyboard layout (KeyBindingName with stand-in AZERTY, QWERTZ and Turkish F layouts)
add_executable(pt_key_layout_test tests/key_layout_test.cpp)
target_link_libraries(pt_key_layout_test PRIVATE pt_engine)

# Game+ tier 2's intercom: its flicker stays under three flashes a second, its lit time, its sounds' levels and its ring's roll
add_executable(pt_intercom_test tests/intercom_test.cpp)
target_link_libraries(pt_intercom_test PRIVATE pt_engine)

add_executable(pt_archive_model_camera_test tests/archive_model_camera_test.cpp)
target_link_libraries(pt_archive_model_camera_test PRIVATE pt_engine)

add_executable(pt_pc_prompt_note_test tests/pc_prompt_note_test.cpp)
target_link_libraries(pt_pc_prompt_note_test PRIVATE pt_engine)

add_executable(pt_pc_settings_refresh_test tests/pc_settings_refresh_test.cpp)
target_link_libraries(pt_pc_settings_refresh_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_pc_settings_refresh_test PRIVATE pt_gpu_d3d12)
endif()

# the hint lines' controls: tokens become the device in use's pictures, a hint names no list of devices in any language
add_executable(pt_prompt_hint_test tests/prompt_hint_test.cpp)
target_link_libraries(pt_prompt_hint_test PRIVATE pt_engine)

# the top banner's lines (update, Game+ tier): one at a time, each for its whole time, never two at once
add_executable(pt_notice_queue_test tests/notice_queue_test.cpp)
target_link_libraries(pt_notice_queue_test PRIVATE pt_engine)
# frame generation losses at start: the previous run's log decides whether the next start keeps frame generation off
add_executable(pt_fg_start_guard_test tests/fg_start_guard_test.cpp)
target_include_directories(pt_fg_start_guard_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(pt_support_link_test tests/support_link_test.cpp)
target_include_directories(pt_support_link_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# the dialogs' buttons (dialog_view.h): the focus starts on the safe choice, the D-pad, the arrows and the pointer move it, accept and a click choose,
# back cancels, Arabic lays the plates out from the right
add_executable(pt_dialog_nav_test tests/dialog_nav_test.cpp)
target_link_libraries(pt_dialog_nav_test PRIVATE pt_engine)

# the Museum's unlock keys: what each exhibit waits for and what a sound, a picture or a demo shown in play opens
add_executable(pt_archive_unlock_test tests/archive_unlock_test.cpp)
target_link_libraries(pt_archive_unlock_test PRIVATE pt_engine)
if(TARGET pt_gpu_d3d12)
  target_link_libraries(pt_archive_unlock_test PRIVATE pt_gpu_d3d12)
endif()

if(WIN32)
  add_executable(pt_discord_presence_test tests/discord_presence_test.cpp)
  target_include_directories(pt_discord_presence_test PRIVATE src)
  target_compile_definitions(pt_discord_presence_test PRIVATE NOMINMAX)
  add_executable(pt_spatial_output_test tests/spatial_output_test.cpp)
  target_include_directories(pt_spatial_output_test PRIVATE src ${glm_SOURCE_DIR})
  target_compile_definitions(pt_spatial_output_test PRIVATE NOMINMAX)
  target_link_libraries(pt_spatial_output_test PRIVATE ole32)
endif()

add_executable(pt_display_modes_test tests/display_modes_test.cpp)
target_link_libraries(pt_display_modes_test PRIVATE pt_engine)
add_executable(pt_display_policy_test tests/display_policy_test.cpp)
target_link_libraries(pt_display_policy_test PRIVATE pt_engine)

add_executable(pt_pipeline_cache_store_test tests/pipeline_cache_store_test.cpp)
target_link_libraries(pt_pipeline_cache_store_test PRIVATE pt_engine)

add_executable(pt_survival_logic_test tests/survival_logic_test.cpp)
target_link_libraries(pt_survival_logic_test PRIVATE pt_engine)

add_executable(pt_key_rebind_test tests/key_rebind_test.cpp)
target_link_libraries(pt_key_rebind_test PRIVATE pt_engine)

