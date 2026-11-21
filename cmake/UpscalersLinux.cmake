# Upscalers of the native Linux build (docs/upscaling.md, Linux), included from cmake/Upscalers.cmake. The Windows block
# there is unchanged; this file only runs for CMAKE_SYSTEM_NAME Linux.
#
# NVIDIA DLSS: the DLSS SDK 310.9.1 ships Linux x86-64 files beside the Windows ones: libnvsdk_ngx.a (linked, the NGX
# loader that finds the driver's libnvidia-ngx.so.1) and the DLSS Super Resolution and Ray Reconstruction runtimes
# libnvidia-ngx-dlss.so.310.9.1 and libnvidia-ngx-dlssd.so.310.9.1, staged next to pt under the same NVIDIA RTX SDKs
# licence as nvngx_dlss.dll and nvngx_dlssd.dll.
#
# AMD FSR 3.1: AMD ships the FidelityFX API only as Windows DLLs, but the SDK is MIT source. libamd_fidelityfx_vk.so is
# built here from the v1.1.4 sources of the FSR 3.1.4 upscaler, the Vulkan backend and the FidelityFX API, with
# third_party/ffx_linux (an MSVC compatibility header, the provider table and the case of two include paths) and three
# small patches below (an alignment, the luma history's format, two switches for testers). The
# Vulkan shader permutations are compiled with AMD's FidelityFX_SC.exe and glslangValidator as on Windows: run directly
# on a Windows host (the cross build), through wine on a Linux host. Frame generation is left out (its Vulkan swapchain
# is written against Win32 threads and events).
#
# Intel XeSS: the XeSS SDK 3.0.2 has Windows DLLs only (its guide lists Windows 10/11), so there is nothing to build.

set(PT_DLSS_LINUX_FILES
  "lib/Linux_x86_64/libnvsdk_ngx.a|3321c4ca9adf71f345550d10bf2148ed38003151b03ec5f81c99d00949ab8202"
  "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1|7561f16cab74e2b7ccf791bf44bb9cc43abb44bcf78f33c5663520ad4e857e02"
  "lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.1|15043b4129a420b09dcd45a0294cb15f135da23054602827f68095f8080b05b0")

# the files the Linux FSR library is built from (the C++ sources, the headers they include, the GLSL passes and what
# they include, the shader compiler and the licence), from tag v1.1.4
set(PT_FFX_SRC_DIR "${PT_UPSCALER_SDK_DIR}/fidelityfx-sdk-v1.1.4-src")
set(PT_FFX_SRC_FILES
  "LICENSE.txt|82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a"
  "ffx-api/include/ffx_api/ffx_api.h|408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561"
  "ffx-api/include/ffx_api/ffx_api.hpp|42470189c0241feca12eed959cf90ade15806bb9ecc578b65f8c20ca9365c34d"
  "ffx-api/include/ffx_api/ffx_api_types.h|dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe"
  "ffx-api/include/ffx_api/ffx_framegeneration.h|344936dccf49267d4421d1d74dc6db617d0a83c5a9f5991841325e812a38f127"
  "ffx-api/include/ffx_api/ffx_upscale.h|1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1"
  "ffx-api/include/ffx_api/ffx_upscale.hpp|6451754c2539a7e991429a7bd8365bb848f51abebad1982715f345989cc0a234"
  "ffx-api/include/ffx_api/vk/ffx_api_vk.h|a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9"
  "ffx-api/src/backends.cpp|877d505a1c2b624c56434c4705c341ff78c0c27493c9a26b0ee01c89b2e5e0b8"
  "ffx-api/src/backends.h|b93c7c41d33e68e50f8bac708780cb42ac9ec53297b15facdf86c7af1f2e7621"
  "ffx-api/src/ffx_api.cpp|95c78f444c31cbeb277ec96417132eaa5f1d6720f685f716b4d50662e742dfb8"
  "ffx-api/src/ffx_provider.h|22adce3eda551c1cc9ed7ad83fe82d6028c80ceabac73686acecb0f0d9700f6b"
  "ffx-api/src/ffx_provider_fsr3upscale.cpp|fed6c5669e2b684f84259b55229b16872ba7a090ef9a656409bfe15430ffd5b8"
  "ffx-api/src/ffx_provider_fsr3upscale.h|3369566775ddf786c232a13ce49fdfaccc7af445f42571a6b5569ec5dbbd1722"
  "ffx-api/src/validation.cpp|1479ceef4d30e15b61a3f25c52fe4f7f1485664f9ca90ca4ae1403f323443430"
  "ffx-api/src/validation.h|a12e1a62a87597eb47b2aa86d289de61a9a32eb62a63d6c8943f9ff3cacb167a"
  "sdk/include/FidelityFX/gpu/ffx_common_types.h|a19560f837c09f0c990e98b5925b731135d908e372af41c5895cbfa70cc9e5c8"
  "sdk/include/FidelityFX/gpu/ffx_core.h|e439aa5f516b2d2ebf070ff1d24561f04fa5f00262a2e4f760764a7d07de7997"
  "sdk/include/FidelityFX/gpu/ffx_core_cpu.h|bdb58574cf3ba80803c4198b4661e3a1cd63c877d59833f22bf15962b164a7fe"
  "sdk/include/FidelityFX/gpu/ffx_core_glsl.h|01afe5b65549b2c0951b494ca4bf570ee2aa89a64344e4c7d98ef86e6613ab42"
  "sdk/include/FidelityFX/gpu/ffx_core_gpu_common.h|0e3c50cb0b4592c051d87e028a1dfa54fbd097942ebb3146964db801dec095d0"
  "sdk/include/FidelityFX/gpu/ffx_core_gpu_common_half.h|d10466cbf027c8c23858a25f8e8cbbbf521d29f373db663c9354221651e9a498"
  "sdk/include/FidelityFX/gpu/ffx_core_hlsl.h|98722d4b1331c437f5aee645e6f12ed722b721f2ffcb0ec523ee817857f394c7"
  "sdk/include/FidelityFX/gpu/ffx_core_portability.h|a7c3a1c213ec083bec401181036cffe7a222af2c430c0bab6bd8c14e83e8a567"
  "sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_resources.h|aa9daea4d768cdc014de54bd42890673ffdef2d7d8cf796c986c2b873ad98ac9"
  "sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h|5d608d9d0645da03a3aa6c729c5ad990c6f222053eab850b8e218607c1baaa9d"
  "sdk/include/FidelityFX/gpu/fsr2/ffx_fsr2_resources.h|455fda0de148f4e2d31f3a10151d0d5312e811fc7f5e5ebdd3acba192abfac0a"
  "sdk/include/FidelityFX/gpu/fsr3/ffx_fsr3_resources.h|3e1e2018b140f078330570313b27fb4a6e99caf8a874ff1a0484795180a64408"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h|7212a028c6ff3ca4a76f9d62b7f64f5f5adce0f108067d8995c7bc795234afbc"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h|875c2b8d8df93716fb46271886c768a9ef40c5ccc11ade39397952b4a4e54321"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h|85d6250e9b89d431167e98400e6e2db64227769b393c97ca6aad077581179114"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_common.h|4898aa014fef875d3644b9b3bed03f31a963349e17ad553198f87ae8af82cb3f"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_debug_view.h|51849bd2f758e2b15375c6a21633fea2f7433a19eb3d74d1d07f8ead36a695ba"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h|82d13d21e161732e48b267d28906f067a8df306ead5aeeed65a1ef66b3e06003"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h|fa6eeaf8197b214ee1853d73da3c66cd0c8fc24444a65237b0023bd68f10d887"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h|acae0499b8805c363c105895afc28fc90e549b38c8bffdcbec1cf9e2f842abb0"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h|ddf7a6d9cd6e357328e972e0b654da977edcb2e337c992b06d6256eeae339e03"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h|f1d8a9152bd3ceda8a10dafe44e0c6d27109b345fc4a5704f71c6bf23d6c5cb4"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h|d2d01ea2f59658e5494414448f11435879bdac871c3ad7c84b951248bc223702"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h|10c6f7619a6c1ff9983fdff9bac565789b1cda52f97ef60b491498223e7b7097"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h|e8de656c71b15537ddd891b7485492b7829e6aec1dea96256fbc246c9a7c7366"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h|13e772ff9bd32244fb990704055040f8dbb945c014cc6f0fdbf553188b300e7e"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h|b1e94d7cbb1b7d6e4556e4a080577a62fde07efbc8379b52223d322ed4b75912"
  "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h|c51f3af61b43fd779a6685202886dbb5dab3bfea989f8b5060c8c9ab4a93207b"
  "sdk/include/FidelityFX/gpu/spd/ffx_spd.h|d276f32ee18948cb053c991f3c8821f9664c93f12cb2483dd2978f333bca67b2"
  "sdk/include/FidelityFX/host/backends/vk/ffx_vk.h|75343f8508cfa609bd79a7deddf2df572f0ff876ead8b5ddd1bd9c46f96f1459"
  "sdk/include/FidelityFX/host/ffx_assert.h|d64fb7b916773d2ac96903fc699e438099b8702ecf5ce7aba051bd7e0a109ee1"
  "sdk/include/FidelityFX/host/ffx_error.h|511ba2b3d99aaa3ac450de7c548244647df146939a7de105bd85713bde4a36c0"
  "sdk/include/FidelityFX/host/ffx_fsr3upscaler.h|686cf613f70a1b01a9fa1302fb15f896b86cf2a68b069b3f5f2b5c0bcd9ea15b"
  "sdk/include/FidelityFX/host/ffx_interface.h|0b012be0256c47663905e51fa31873f8bf2c5536c40211e1b01cf117eb5988d6"
  "sdk/include/FidelityFX/host/ffx_message.h|48c125292b054be1ad208e36b24fb174a9cae80a3fd780a2cb52c4674e6bf4fe"
  "sdk/include/FidelityFX/host/ffx_types.h|057535d46b7241bb25163a21dc4e59b0029f2d3da6a474b1c1028f921d5eb6f8"
  "sdk/include/FidelityFX/host/ffx_util.h|efe50400378277356116f0a3989ec69367b08aaae65892fdf3a4cf931a4c7bb8"
  "sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp|a078056425c94ca52c1727e8dee6b2c8fca1fe4c2ad891eaed0cb5a5a6b585bf"
  "sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.h|8891aa48a3c515684f80ba17c004b3634439db101f10c09535b9cf3626395381"
  "sdk/src/backends/shared/ffx_shader_blobs.cpp|cdd8bd85bec136aca6c3dcb0e51ab7152b7af093445b14786a7ab7bd3b5c164a"
  "sdk/src/backends/shared/ffx_shader_blobs.h|d139f399b80ebf2f232c3fd64139edd1ea62f4d5f28ff144c3971fffdc5771a4"
  "sdk/src/backends/vk/ffx_vk.cpp|9fa430949184fadb357e6949d7564cf0f8e4653ead683a99755a6f15c4a76589"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_accumulate_pass.glsl|73b5be6db479475559d2374957b2854aa6d7f7ddadba76ef5e4ed7f248ea9564"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_autogen_reactive_pass.glsl|95991f97c70409cd2783ce7b6866d1fd318e8751c774e1aef6b55f3e470bd81f"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_debug_view_pass.glsl|2ed1674f3f42316b1603111155aa042096158a646cb59117d38b6b0ec130c78b"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_instability_pass.glsl|423345d36c811237af7a41ef5e6220638f5159952e90fa084aa952ddd267581b"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid_pass.glsl|3e637a702c2cc6b6a222df4327a4ed565f731c6190e958e0ffec5b43741f7ea4"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs_pass.glsl|bdf3b1ca0fdce968afb79f55d667960382139926eccdfdec46e08dbae3982657"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity_pass.glsl|176f6aa5f2f437487222c7fc057ce858795a6677d6ac3e937a1ce36521c6ddfc"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_rcas_pass.glsl|110b409d5e10b16e5a237c13932a707c1c07932bf790e814f1a9d76f02569ad7"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pass.glsl|246d24b76e292b7c8c5ba39dfc3a4dff1486701f5fccc6482284afac5a7daaf9"
  "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl|ce34582f454925499523d30d2619e19aefd8f1c091545c37d4ebd8feea91cf55"
  "sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp|0b1f1227535971a818c6367f1c27f115db682aea4371fa0c15e8dbd3b0ba5764"
  "sdk/src/components/fsr3upscaler/ffx_fsr3upscaler_private.h|89935ce16720ce8b164538e886df29929716e947b47c72ca3243fed833286bdd"
  "sdk/src/shared/ffx_assert.cpp|c84efb3fe942e3c80b6a595cc68e580154d2380717a88f2ac37939e156b27952"
  "sdk/src/shared/ffx_breadcrumbs_list.cpp|fbe34fcdda7db272b7cdb97619ba4d800829fcbabd42cac6e9862d7f898de14a"
  "sdk/src/shared/ffx_breadcrumbs_list.h|06c6a58888cbb0d9a74b330682adf3ad0a50d4aea8bce8e69e52c2e5b9c724e6"
  "sdk/src/shared/ffx_message.cpp|96c5a2d576f0688aa77249bf9a85ff8ce3e85c874ccc3d7d4201693f95818811"
  "sdk/src/shared/ffx_object_management.cpp|607b93de58b16eaa891056b6664d719699c83f8ba9e039aa1a9e44fcecd1ab09"
  "sdk/src/shared/ffx_object_management.h|e388d1af28f4d3f7388e524257b9692be867884cff720414031d9768995bf223"
  "sdk/tools/binary_store/FidelityFX_SC.exe|75480f2245e7b2cac300b013fad1453d4b966e5bd52c69205a40537de05f02a9")

# DLSS: the headers of the Windows list plus the Linux libraries
set(pt_dlss_linux_entries ${PT_DLSS_LINUX_FILES})
foreach(entry ${PT_DLSS_FILES})
  if(entry MATCHES "^(include/|LICENSE)")
    list(APPEND pt_dlss_linux_entries "${entry}")
  endif()
endforeach()
pt_sdk_files(pt_dlss_ok "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1" "${PT_DLSS_DIR}" ${pt_dlss_linux_entries})
if(pt_dlss_ok)
  target_include_directories(pt_engine PRIVATE "${PT_DLSS_DIR}/include")
  target_compile_definitions(pt_engine PRIVATE PT_WITH_DLSS=1)
  # libnvsdk_ngx.a uses dlopen and the C++ runtime (linked statically into pt, CMakeLists.txt)
  target_link_libraries(pt_engine PRIVATE "${PT_DLSS_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a" ${CMAKE_DL_LIBS})
  list(APPEND PT_UPSCALER_RUNTIME_FILES "${PT_DLSS_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1"
    "${PT_DLSS_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.1")
  list(APPEND PT_UPSCALER_NOTICES "${PT_DLSS_DIR}/LICENSE.txt|NVIDIA_RTX_SDKs_LICENSE.txt")
endif()

# FSR: the sources, then the shader compiler on this host
pt_sdk_files(pt_ffx_src_ok "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4" "${PT_FFX_SRC_DIR}"
  ${PT_FFX_SRC_FILES})
find_program(PT_GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
set(pt_ffx_sc "${PT_FFX_SRC_DIR}/sdk/tools/binary_store/FidelityFX_SC.exe")
set(pt_ffx_sc_command)
if(CMAKE_HOST_WIN32)
  set(pt_ffx_sc_command "${pt_ffx_sc}")
else()
  find_program(PT_WINE wine)
  if(PT_WINE)
    set(pt_ffx_sc_command "${PT_WINE}" "${pt_ffx_sc}")
  endif()
endif()
if(NOT pt_ffx_src_ok)
  message(WARNING "FSR on Linux: the FidelityFX SDK sources could not be downloaded; FSR is left out")
elseif(NOT PT_GLSLANG_VALIDATOR)
  message(WARNING "FSR on Linux: glslangValidator not found (Vulkan SDK or glslang-tools); FSR is left out")
elseif(NOT pt_ffx_sc_command)
  message(WARNING "FSR on Linux: AMD's shader compiler FidelityFX_SC.exe needs wine on a Linux host; FSR is left out")
else()
  set(pt_ffx_ok TRUE)
  set(ffx "${PT_FFX_SRC_DIR}")
  set(gpu "${ffx}/sdk/include/FidelityFX/gpu")
  set(gen "${CMAKE_BINARY_DIR}/generated/ffx_fsr3upscaler")
  get_filename_component(glslang_dir "${PT_GLSLANG_VALIDATOR}" DIRECTORY)
  # The one change to AMD's shaders: ffx_fsr3upscaler_callbacks_glsl.h declares the luma history storage image rgba8, but the
  # upscaler creates it R16G16B16A16_FLOAT (ffx_fsr3upscaler.cpp, LumaHistory1 and 2) and the luma instability pass keeps luma
  # divided by the exposure in it, far above 1. A format qualifier that differs from the view's format gives undefined values
  # to the whole image (the validation layer reports it on every frame, with AMD's own Windows DLL too). NVIDIA's driver
  # stores by the view's format; Mesa's RADV compiles stores by the qualifier (it shortened the stores of an FSR shader with
  # the same kind of mistake in World War Z, Mesa merge request 13084). The patched copy of the header is searched before
  # AMD's include folder and says rgba16f.
  set(ffx_glsl_callbacks "${gpu}/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h")
  set(ffx_glsl_dir "${CMAKE_BINARY_DIR}/generated/ffx_linux_glsl")
  set(ffx_glsl_patched "${ffx_glsl_dir}/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h")
  file(READ "${ffx_glsl_callbacks}" ffx_glsl_text)
  string(REPLACE "binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8) uniform image2D  rw_luma_history;"
    "binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f) uniform image2D  rw_luma_history; // P.T. port, Linux"
    ffx_glsl_text "${ffx_glsl_text}")
  if(NOT ffx_glsl_text MATCHES "rgba16f\\) uniform image2D  rw_luma_history; // P\\.T\\. port, Linux")
    message(FATAL_ERROR "FSR on Linux: the luma history patch did not apply to ${ffx_glsl_callbacks}")
  endif()
  set(ffx_glsl_existing "")
  if(EXISTS "${ffx_glsl_patched}")
    file(READ "${ffx_glsl_patched}" ffx_glsl_existing)
  endif()
  if(NOT ffx_glsl_existing STREQUAL ffx_glsl_text)
    file(WRITE "${ffx_glsl_patched}" "${ffx_glsl_text}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ffx_glsl_callbacks}")
  # the arguments of the SDK's CMakeShadersFSR3Upscaler.txt and CMakeCompileFSR3UpscalerShaders.txt
  set(base -reflection -deps=gcc -DFFX_GPU=1
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}" "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}" "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}" "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
    "-I${ffx_glsl_dir}" "-I${gpu}" "-I${gpu}/fsr3upscaler" "-output=${gen}")
  set(permutations)
  file(GLOB passes "${ffx}/sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl")
  foreach(pass ${passes})
    get_filename_component(name "${pass}" NAME_WE)
    foreach(variant "_:0" "_wave64:0" "_16bit:1" "_wave64_16bit:1")
      string(REPLACE ":" ";" variant "${variant}")
      list(GET variant 0 suffix)
      list(GET variant 1 half)
      if(suffix STREQUAL "_")
        set(suffix "")
      endif()
      set(header "${gen}/${name}${suffix}_permutations.h")
      add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen}"
        COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_prepend:${glslang_dir}" -- ${pt_ffx_sc_command} ${base}
          "-name=${name}${suffix}" "-DFFX_HALF=${half}" "${pass}"
        DEPENDS "${pass}" "${ffx_glsl_patched}" VERBATIM)
      list(APPEND permutations "${header}")
    endforeach()
  endforeach()
  add_custom_target(pt_ffx_fsr3upscaler_shaders DEPENDS ${permutations})
  # The changes to AMD's code. The Vulkan backend carves its scratch memory into arrays at 4-byte steps, and its
  # EffectContext is alignas(32). MSVC never relied on that alignment; clang stores into it with aligned SSE moves, which
  # fault where wchar_t (4 bytes here, 2 on Windows) has moved the array off a 32-byte boundary (seen in WSL1 with lavapipe:
  # movdqa in CreateBackendContextVK). The patched copy aligns that one array and reserves the padding in the size.
  # And the upscaler picks its shaders by the device (ffx_fsr3upscaler.cpp): a subgroup size range of 32 to 64 (AMD)
  # reprojects with the Lanczos table, float16 runs the 16-bit shaders, both of them unlike the NVIDIA path this library was
  # checked on. PT_FSR_NO_LUT and PT_FSR_NO_FP16 make it pick the other set, for a tester whose driver draws one of them
  # wrong (docs/upscaling.md, Linux tester checklist).
  set(ffx_vk_source "${ffx}/sdk/src/backends/vk/ffx_vk.cpp")
  set(ffx_vk_patched "${CMAKE_BINARY_DIR}/generated/ffx_linux_src/ffx_vk.cpp")
  file(READ "${ffx_vk_source}" ffx_vk_text)
  set(ffx_vk_original "${ffx_vk_text}")
  string(REPLACE "        // Map context array
        backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem;"
    "        // Map context array
        pMem = (uint8_t*)FFX_ALIGN_UP((uintptr_t)pMem, alignof(BackendContext_VK::EffectContext)); // P.T. port, Linux
        backendContext->pEffectContexts = (BackendContext_VK::EffectContext*)pMem;"
    ffx_vk_text "${ffx_vk_text}")
  string(REPLACE "                            pipelineArraySize + resourceArraySize + contextArraySize,"
    "                            pipelineArraySize + resourceArraySize + contextArraySize + alignof(BackendContext_VK::EffectContext), // P.T. port, Linux"
    ffx_vk_text "${ffx_vk_text}")
  string(REPLACE "            deviceCapabilities->waveLaneCountMax = subgroupSizeControlProperties.maxSubgroupSize;"
    "            deviceCapabilities->waveLaneCountMax = subgroupSizeControlProperties.maxSubgroupSize;
            if (getenv(\"PT_FSR_NO_LUT\")) deviceCapabilities->waveLaneCountMax = deviceCapabilities->waveLaneCountMin; // P.T. port, Linux"
    ffx_vk_text "${ffx_vk_text}")
  string(REPLACE "            deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16;"
    "            deviceCapabilities->fp16Supported = (bool)shaderFloat18Int8Features.shaderFloat16 && !getenv(\"PT_FSR_NO_FP16\"); // P.T. port, Linux"
    ffx_vk_text "${ffx_vk_text}")
  string(REGEX MATCHALL "// P\.T\. port, Linux" ffx_vk_marks "${ffx_vk_text}")
  list(LENGTH ffx_vk_marks ffx_vk_mark_count)
  if(NOT ffx_vk_mark_count EQUAL 4)
    message(FATAL_ERROR "FSR on Linux: the patches did not apply to ${ffx_vk_source}")
  endif()
  set(ffx_vk_existing "")
  if(EXISTS "${ffx_vk_patched}")
    file(READ "${ffx_vk_patched}" ffx_vk_existing)
  endif()
  if(NOT ffx_vk_existing STREQUAL ffx_vk_text)
    file(WRITE "${ffx_vk_patched}" "${ffx_vk_text}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ffx_vk_source}")
  set(glue "${CMAKE_SOURCE_DIR}/third_party/ffx_linux")
  add_library(pt_ffx_vk SHARED
    "${ffx}/ffx-api/src/ffx_api.cpp" "${ffx}/ffx-api/src/ffx_provider_fsr3upscale.cpp" "${ffx}/ffx-api/src/validation.cpp"
    "${ffx}/ffx-api/src/backends.cpp" "${ffx_vk_patched}"
    "${ffx}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp" "${ffx}/sdk/src/shared/ffx_assert.cpp"
    "${ffx}/sdk/src/shared/ffx_message.cpp" "${ffx}/sdk/src/shared/ffx_object_management.cpp"
    "${ffx}/sdk/src/shared/ffx_breadcrumbs_list.cpp" "${ffx}/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "${ffx}/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp" "${glue}/ffx_provider_linux.cpp")
  add_dependencies(pt_ffx_vk pt_ffx_fsr3upscaler_shaders)
  set_target_properties(pt_ffx_vk PROPERTIES OUTPUT_NAME amd_fidelityfx_vk LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
  target_include_directories(pt_ffx_vk PRIVATE "${ffx}/ffx-api/include" "${ffx}/ffx-api/src" "${ffx}/sdk/include"
    "${ffx}/sdk/src/shared" "${ffx}/sdk/src/backends/shared" "${ffx}/sdk/src/backends/shared/blob_accessors"
    "${ffx}/sdk/src/components" "${gen}")
  # the glue include folder is searched last: only a case-sensitive file system reaches its FidelityFx/ (lower-case x)
  # forwarders
  target_compile_options(pt_ffx_vk PRIVATE -idirafter "${glue}/include" -include "${glue}/ffx_linux_compat.h" -fms-extensions -w)
  target_compile_definitions(pt_ffx_vk PRIVATE FFX_BACKEND_VK FFX_FSR3UPSCALER NDEBUG)
  # only the five FidelityFX API entry points leave the library
  target_link_options(pt_ffx_vk PRIVATE "-Wl,--version-script=${glue}/ffx_api.map" -Wl,--no-undefined)
  target_link_libraries(pt_ffx_vk PRIVATE Vulkan::Vulkan)
  foreach(game_target pt pt_release)
    if(TARGET ${game_target})
      add_dependencies(${game_target} pt_ffx_vk)
    endif()
  endforeach()
  target_include_directories(pt_engine PRIVATE "${ffx}/ffx-api/include")
  target_compile_definitions(pt_engine PRIVATE PT_WITH_FSR=1)
  list(APPEND PT_UPSCALER_NOTICES "${ffx}/LICENSE.txt|AMD_FidelityFX_SDK_LICENSE.txt")
endif()
