set(PT_UPSCALER_SDK_DIR "${CMAKE_BINARY_DIR}/_deps/upscaler-sdks" CACHE PATH "Folder for the downloaded upscaler SDK files")
# FSR (amd_fidelityfx_vk.dll), XeSS (libxess.dll) and the DLSS link library used here are Windows binaries: off elsewhere
if(WIN32)
  set(pt_upscalers_default ON)
else()
  set(pt_upscalers_default OFF)
endif()
option(PT_UPSCALERS "Download the FSR, DLSS and XeSS SDK files and build the optional upscalers" ${pt_upscalers_default})
option(PT_REQUIRE_UPSCALERS "Fail Windows release configuration unless every upscaler backend and runtime is available" OFF)
include("${CMAKE_SOURCE_DIR}/cmake/check_upscaler_policy.cmake")

function(pt_sdk_files out_ok base_url dir)
  set(ok TRUE)
  foreach(entry ${ARGN})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    set(path "${dir}/${file}")
    if(EXISTS "${path}")
      file(SHA256 "${path}" existing)
      if(NOT existing STREQUAL hash)
        file(REMOVE "${path}")
      endif()
    endif()
    if(NOT EXISTS "${path}")
      message(STATUS "downloading ${base_url}/${file}")
      file(DOWNLOAD "${base_url}/${file}" "${path}.part" STATUS status)
      list(GET status 0 code)
      if(code EQUAL 0)
        file(SHA256 "${path}.part" downloaded)
        if(downloaded STREQUAL hash)
          file(RENAME "${path}.part" "${path}")
        else()
          set(status "hash mismatch ${downloaded}")
        endif()
      endif()
      if(NOT EXISTS "${path}")
        file(REMOVE "${path}.part")
        message(WARNING "cannot download ${base_url}/${file}: ${status}")
        set(ok FALSE)
      endif()
    endif()
  endforeach()
  set(${out_ok} ${ok} PARENT_SCOPE)
endfunction()

set(PT_FFX_DIR "${PT_UPSCALER_SDK_DIR}/fidelityfx-sdk-v1.1.4")
set(PT_FFX_FILES
  "LICENSE.txt|82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a"
  "PrebuiltSignedDLL/amd_fidelityfx_vk.dll|a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78"
  "ffx-api/include/ffx_api/ffx_api.h|408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561"
  "ffx-api/include/ffx_api/ffx_api_loader.h|7be51b7bf1e640b84c0c7f22a9db50a6816debce3755b51224e56c89501659c3"
  "ffx-api/include/ffx_api/ffx_api_types.h|dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe"
  "ffx-api/include/ffx_api/ffx_upscale.h|1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1"
  "ffx-api/include/ffx_api/ffx_framegeneration.h|344936dccf49267d4421d1d74dc6db617d0a83c5a9f5991841325e812a38f127"
  "ffx-api/include/ffx_api/vk/ffx_api_vk.h|a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9")

set(PT_DLSS_DIR "${PT_UPSCALER_SDK_DIR}/dlss-v310.9.1")
set(PT_DLSS_FILES
  "LICENSE.txt|d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
  "include/nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
  "include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
  "include/nvsdk_ngx_defs_vk.h|0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"
  "include/nvsdk_ngx_helpers.h|5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
  "include/nvsdk_ngx_helpers_vk.h|c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"
  "include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
  "include/nvsdk_ngx_vk.h|2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"
  "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3"
  "lib/Windows_x86_64/rel/nvngx_dlss.dll|3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983")

set(PT_XESS_DIR "${PT_UPSCALER_SDK_DIR}/xess-v3.0.2")
set(PT_XESS_FILES
  "LICENSE.txt|784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df"
  "inc/xess/xess.h|98dc435f5aed252d83a161fc1f86223f20c67eb863214fe3b5cb215a3f6e3319"
  "inc/xess/xess_vk.h|859086df3c531ceccab628b287f69bd9a4528045bcbdd8c639589082dcbf8977"
  "bin/libxess.dll|251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7")

# NVIDIA Streamline 2.14.1 (github.com/NVIDIA-RTX/Streamline, release zip): DLSS Frame Generation, Reflex and PCL, and DLSS
# Super Resolution through sl.dlss while Streamline is loaded (upscaling.md, DLSS Frame Generation). Only the production
# (signed) DLLs of bin/x64 and the headers are taken from the 276 MB zip, which is deleted after extraction. Experimental:
# a release build configured with -DPT_STREAMLINE=OFF leaves the code and the DLLs out.
option(PT_STREAMLINE "Download Streamline and build the experimental DLSS Frame Generation path" ${pt_upscalers_default})
set(PT_SL_DIR "${PT_UPSCALER_SDK_DIR}/streamline-v2.14.1")
set(PT_SL_ZIP_HASH "92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b")
set(PT_SL_FILES
  "license.txt|7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d"
  "bin/x64/nvngx_dlss.license.txt|3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294"
  "bin/x64/reflex.license.txt|ebf83c07fb3b2939908c3795d887afde3161c89a28ba391724efc784ce1bdabe"
  "include/sl.h|e1e81a7428d15b30db37587e9469bd68a56d630d820a4accec0aee3b17e157dd"
  "include/sl_consts.h|17de74afda2cb96204ff380464bed926f660d92015fe0304dff4e329509c88b9"
  "include/sl_core_api.h|328dc3a2c1dee579c200ca97e2d5b1ec38c893be4ba2745ef5ad506af7172e81"
  "include/sl_core_types.h|f420cb052ce14489fc9fa0d933c3ed81a010deb01fa13b1aa8d45da1c5ba3234"
  "include/sl_dlss.h|d2c8c61fa71794c8ba424cebce5b3079ebfab8e47ed98b4b00cf8b257ecfd6b5"
  "include/sl_dlss_g.h|1fc18cbe004e280df1f787276d08a1b28b8a8c4c65856fbaa659f56dff6a915d"
  "include/sl_reflex.h|3b623a1189e04a686384d224a58c4ad9974c4e6e3204077676f6ec529475164c"
  "include/sl_pcl.h|f43c5135fc8d5349ccd345e424f8cf1f61953a9f17e9897204b0741a3ab7b0fb"
  "include/sl_helpers_vk.h|82604567d239d00ac0ac2270cec744f6f11266e1d9d5187a95d4954f76d8c41c"
  "include/sl_helpers.h|48629edbf25dfdb6444c91235fc278f0f9a5878243fae83204c5906ce9eacd8a"
  "include/sl_struct.h|28deda67ea1a74371dd4f33ff4842b7db03b2db007001da6db06f6982297928d"
  "include/sl_version.h|48e9cb86f0ff6711304bdf3f7150f46466878bbefc21f648773267bc6a326285"
  "include/sl_result.h|2a0f6c12863bdc00b38910a5ec85d1f083c5671ee817a920afe626ac2a9100f7"
  "include/sl_appidentity.h|1337385ac9867d66fa6beb34c750e79aaf25a74a156a47e83f24937299dade87"
  "include/sl_device_wrappers.h|af7741305b1a468c3a83efaa2005ecd53d2ffd7aadb8ec6868c9dcb4a8a8e3a1"
  "include/sl_security.h|a36b1c20394402bee7ce738089a5a1305ff6e9dd9dd517566154ce834f7c32b4"
  "include/sl_hooks.h|cfeff70a52e1cc012cf4c955a15cc9d0b290f7e084dcb3c0168a9af06ff8f122"
  "include/sl_matrix_helpers.h|a65758d85abba1e12845d266d7d5e36aaefa952383cdf093f3143cd6d8653341"
  "bin/x64/sl.interposer.dll|8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c"
  "bin/x64/sl.common.dll|82924a8954dd671e09351c5de0eb87ad0eb25b944cc9f9ab955ca1d9950de15d"
  "bin/x64/sl.dlss.dll|73bf52c0cfaa5900a8f3f4a91306e4625e7cca696dfb305aae44c9f97b582e1f"
  "bin/x64/sl.dlss_g.dll|f4a6b2b14dcc0b1485989e430d3b4e3a44ac1800b92ba1ad74f476e64fb2b09c"
  "bin/x64/sl.reflex.dll|0ce9725e3e03ea9e7f81d008b57f33ee365973d2e349131c8b1c3e3378fe2db0"
  "bin/x64/sl.pcl.dll|f13d51cfa05f4cd514df2026049e2db8adf359221713170ad386fd499915b582"
  "bin/x64/nvngx_dlssg.dll|ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82")
# the DLLs that ship: the interposer and sl.common (mandatory), the plugins used, and the DLSS-G runtime. nvngx_dlss.dll
# comes from the DLSS SDK above (the same 310.9.1 file as Streamline's). NvLowLatencyVk.dll is left out: sl.reflex uses
# VK_NV_low_latency2 where the driver has it (Streamline 2.14.0), and the Reflex license names no such DLL as distributable.
set(PT_SL_RUNTIME sl.interposer.dll sl.common.dll sl.dlss.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)

function(pt_streamline_files out_ok)
  set(missing FALSE)
  foreach(entry ${PT_SL_FILES})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    set(path "${PT_SL_DIR}/${file}")
    if(EXISTS "${path}")
      file(SHA256 "${path}" existing)
      if(NOT existing STREQUAL hash)
        set(missing TRUE)
      endif()
    else()
      set(missing TRUE)
    endif()
  endforeach()
  if(missing)
    set(zip "${PT_UPSCALER_SDK_DIR}/streamline-sdk-v2.14.1.zip")
    set(url "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip")
    message(STATUS "downloading ${url} (276 MB)")
    file(DOWNLOAD "${url}" "${zip}.part" STATUS status EXPECTED_HASH SHA256=${PT_SL_ZIP_HASH})
    list(GET status 0 code)
    if(code EQUAL 0)
      set(patterns)
      foreach(entry ${PT_SL_FILES})
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 file)
        list(APPEND patterns "${file}")
      endforeach()
      file(ARCHIVE_EXTRACT INPUT "${zip}.part" DESTINATION "${PT_SL_DIR}" PATTERNS ${patterns})
    else()
      message(WARNING "cannot download ${url}: ${status}")
    endif()
    file(REMOVE "${zip}.part")
  endif()
  set(ok TRUE)
  foreach(entry ${PT_SL_FILES})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    set(path "${PT_SL_DIR}/${file}")
    set(existing "")
    if(EXISTS "${path}")
      file(SHA256 "${path}" existing)
    endif()
    if(NOT existing STREQUAL hash)
      message(WARNING "Streamline file ${file} is missing or differs")
      set(ok FALSE)
    endif()
  endforeach()
  set(${out_ok} ${ok} PARENT_SCOPE)
endfunction()

set(PT_UPSCALER_RUNTIME_FILES)
set(PT_UPSCALER_NOTICES)
set(pt_ffx_ok FALSE)
set(pt_dlss_ok FALSE)
set(pt_xess_ok FALSE)
set(pt_sl_ok FALSE)
if(PT_UPSCALERS)
  # what each runtime's licence lets the port ship (licenses/UPSCALERS.txt next to pt.exe)
  list(APPEND PT_UPSCALER_NOTICES "${CMAKE_SOURCE_DIR}/third_party/upscaler_notices/UPSCALERS.txt|UPSCALERS.txt")
  pt_sdk_files(pt_ffx_ok "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4" "${PT_FFX_DIR}" ${PT_FFX_FILES})
  if(pt_ffx_ok)
    target_include_directories(pt_engine PRIVATE "${PT_FFX_DIR}/ffx-api/include")
    target_compile_definitions(pt_engine PRIVATE PT_WITH_FSR=1)
    list(APPEND PT_UPSCALER_RUNTIME_FILES "${PT_FFX_DIR}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll")
    list(APPEND PT_UPSCALER_NOTICES "${PT_FFX_DIR}/LICENSE.txt|AMD_FidelityFX_SDK_LICENSE.txt")
  endif()
  pt_sdk_files(pt_dlss_ok "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1" "${PT_DLSS_DIR}" ${PT_DLSS_FILES})
  if(pt_dlss_ok)
    target_include_directories(pt_engine PRIVATE "${PT_DLSS_DIR}/include")
    target_compile_definitions(pt_engine PRIVATE PT_WITH_DLSS=1)
    target_link_libraries(pt_engine PRIVATE "${PT_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    list(APPEND PT_UPSCALER_RUNTIME_FILES "${PT_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
    list(APPEND PT_UPSCALER_NOTICES "${PT_DLSS_DIR}/LICENSE.txt|NVIDIA_RTX_SDKs_LICENSE.txt")
  endif()
  pt_sdk_files(pt_xess_ok "https://raw.githubusercontent.com/intel/xess/v3.0.2" "${PT_XESS_DIR}" ${PT_XESS_FILES})
  if(PT_REQUIRE_UPSCALERS AND (NOT pt_ffx_ok OR NOT pt_dlss_ok OR NOT pt_xess_ok))
    message(FATAL_ERROR "Strict Windows release requires the FSR, DLSS, and XeSS SDK files; see download/hash errors above")
  endif()
  if(pt_xess_ok)
    target_include_directories(pt_engine PRIVATE "${PT_XESS_DIR}/inc")
    target_compile_definitions(pt_engine PRIVATE PT_WITH_XESS=1)
    list(APPEND PT_UPSCALER_RUNTIME_FILES "${PT_XESS_DIR}/bin/libxess.dll")
    list(APPEND PT_UPSCALER_NOTICES "${PT_XESS_DIR}/LICENSE.txt|Intel_XeSS_SDK_LICENSE.txt")
  endif()
  if(PT_STREAMLINE AND pt_dlss_ok)
    pt_streamline_files(pt_sl_ok)
    if(pt_sl_ok)
      target_include_directories(pt_engine PRIVATE "${PT_SL_DIR}/include")
      target_compile_definitions(pt_engine PRIVATE PT_WITH_STREAMLINE=1)
      foreach(dll ${PT_SL_RUNTIME})
        list(APPEND PT_UPSCALER_RUNTIME_FILES "${PT_SL_DIR}/bin/x64/${dll}")
      endforeach()
      list(APPEND PT_UPSCALER_NOTICES "${PT_SL_DIR}/license.txt|NVIDIA_Streamline_LICENSE.txt"
        "${PT_SL_DIR}/bin/x64/nvngx_dlss.license.txt|NVIDIA_Streamline_DLSS_LICENSE.txt"
        "${PT_SL_DIR}/bin/x64/reflex.license.txt|NVIDIA_Reflex_LICENSE.txt")
    endif()
    if(PT_REQUIRE_UPSCALERS AND NOT pt_sl_ok)
      message(FATAL_ERROR "Windows release requested DLSS Frame Generation but its Streamline runtime set is incomplete")
    endif()
  endif()
endif()

# The package builder checks this configure-time record along with the copied runtime DLLs.
foreach(backend FSR DLSS XeSS Streamline)
  if(backend STREQUAL "FSR")
    set(ok ${pt_ffx_ok})
  elseif(backend STREQUAL "DLSS")
    set(ok ${pt_dlss_ok})
  elseif(backend STREQUAL "XeSS")
    set(ok ${pt_xess_ok})
  else()
    set(ok ${pt_sl_ok})
  endif()
  if(ok)
    set(${backend}_available 1)
  else()
    set(${backend}_available 0)
  endif()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/upscalers.txt" "FSR=${FSR_available}\nDLSS=${DLSS_available}\nXeSS=${XeSS_available}\nStreamline=${Streamline_available}\n")

# These outputs are build dependencies, so Ninja recopies a DLL if it was deleted even when the EXE is up to date.
set(pt_upscaler_runtime_dir "${CMAKE_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
  set(pt_upscaler_runtime_dir "${CMAKE_BINARY_DIR}/$<CONFIG>")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/StageRuntimeFiles.cmake")
set(pt_upscaler_runtime_outputs)
foreach(runtime ${PT_UPSCALER_RUNTIME_FILES})
  get_filename_component(runtime_name "${runtime}" NAME)
  set(runtime_output "${pt_upscaler_runtime_dir}/${runtime_name}")
  pt_add_staged_file(pt_upscaler_runtime_outputs "${runtime}" "${runtime_output}")
endforeach()
foreach(entry ${PT_UPSCALER_NOTICES})
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 notice_source)
  list(GET parts 1 notice_name)
  set(notice_output "${CMAKE_BINARY_DIR}/licenses/${notice_name}")
  pt_add_staged_file(pt_upscaler_runtime_outputs "${notice_source}" "${notice_output}")
endforeach()
add_custom_target(pt_upscaler_runtime_files DEPENDS ${pt_upscaler_runtime_outputs})
foreach(game_target pt pt_release)
  if(TARGET ${game_target})
    add_dependencies(${game_target} pt_upscaler_runtime_files)
  endif()
endforeach()

# Bind the backend record to the linked executable. Packaging rejects stale or replaced binaries.
if(WIN32)
  foreach(target pt pt_release)
    if(TARGET ${target})
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
          "-DEXE=$<TARGET_FILE:${target}>"
          "-DOUT=$<TARGET_FILE:${target}>.upscalers.json"
          "-DFSR=${FSR_available}" "-DDLSS=${DLSS_available}" "-DXeSS=${XeSS_available}"
          "-DStreamline=${Streamline_available}"
          -P "${CMAKE_SOURCE_DIR}/cmake/write_upscaler_manifest.cmake"
        VERBATIM)
    endif()
  endforeach()
endif()
