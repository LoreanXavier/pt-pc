set(PT_TEXTURE_RUNTIME_DIR "${CMAKE_BINARY_DIR}/_deps/texture-runtime" CACHE PATH "Real-ESRGAN runtime cache")
# the upscaler's own release build for the target platform (the Windows one needs vcomp140.dll beside it)
if(WIN32)
  set(pt_texture_flavor windows)
  set(pt_texture_sha256 1bbbdb12d470af80b035c773682e144c6c2f6ece9210832a289af0a48ce3fa9a)
  set(pt_texture_exe realesrgan-ncnn-vulkan.exe)
else()
  set(pt_texture_flavor ubuntu)
  set(pt_texture_sha256 d0e8e1cf954f5cde11be4745dd912cc3774bef36f71c5b1cb8f74c4112b6e919)
  set(pt_texture_exe realesrgan-ncnn-vulkan)
endif()
set(pt_texture_zip "${PT_TEXTURE_RUNTIME_DIR}/runtime-${pt_texture_flavor}.zip")
if(WIN32 AND NOT EXISTS "${pt_texture_zip}" AND EXISTS "${PT_TEXTURE_RUNTIME_DIR}/runtime.zip")
  # caches from before the platform split
  file(RENAME "${PT_TEXTURE_RUNTIME_DIR}/runtime.zip" "${pt_texture_zip}")
endif()
if(NOT EXISTS "${pt_texture_zip}")
  file(DOWNLOAD https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases/download/v0.2.0/realesrgan-ncnn-vulkan-v0.2.0-${pt_texture_flavor}.zip
    "${pt_texture_zip}" EXPECTED_HASH SHA256=${pt_texture_sha256} STATUS pt_texture_download)
  list(GET pt_texture_download 0 pt_texture_download_code)
  if(NOT pt_texture_download_code EQUAL 0)
    file(REMOVE "${pt_texture_zip}")
    message(FATAL_ERROR "Cannot download enhanced texture runtime: ${pt_texture_download}")
  endif()
endif()
set_property(TARGET pt APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_SOURCE_DIR}/third_party/texture_models/realesr-general-x4v3-x2.bin"
  "${CMAKE_SOURCE_DIR}/third_party/texture_models/realesr-general-x4v3-x2.param"
  "${CMAKE_SOURCE_DIR}/third_party/texture_models/LICENSE")
file(GLOB pt_texture_notices CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/texture_notices/*")
set_property(TARGET pt APPEND PROPERTY LINK_DEPENDS ${pt_texture_notices})
file(SHA256 "${pt_texture_zip}" pt_texture_hash)
if(NOT pt_texture_hash STREQUAL "${pt_texture_sha256}")
  message(FATAL_ERROR "Enhanced texture runtime archive checksum mismatch")
endif()
set(pt_texture_runtime "${PT_TEXTURE_RUNTIME_DIR}/realesrgan-ncnn-vulkan-v0.2.0-${pt_texture_flavor}")
if(NOT EXISTS "${pt_texture_runtime}/${pt_texture_exe}")
  file(ARCHIVE_EXTRACT INPUT "${pt_texture_zip}" DESTINATION "${PT_TEXTURE_RUNTIME_DIR}")
endif()
add_custom_command(TARGET pt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pt>/texture-tools/models" "$<TARGET_FILE_DIR:pt>/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pt_texture_runtime}/${pt_texture_exe}" "$<TARGET_FILE_DIR:pt>/texture-tools/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/third_party/texture_models" "$<TARGET_FILE_DIR:pt>/texture-tools/models"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/third_party/texture_notices" "$<TARGET_FILE_DIR:pt>/licenses/enhanced-textures"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pt_texture_runtime}/LICENSE" "$<TARGET_FILE_DIR:pt>/licenses/Real-ESRGAN-ncnn-vulkan.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/texture_models/LICENSE" "$<TARGET_FILE_DIR:pt>/licenses/Real-ESRGAN-model.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bc7enc_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:pt>/licenses/bc7enc.txt" VERBATIM)
if(WIN32)
  add_custom_command(TARGET pt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pt_texture_runtime}/vcomp140.dll" "$<TARGET_FILE_DIR:pt>/texture-tools/" VERBATIM)
endif()
