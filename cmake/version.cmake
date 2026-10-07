# Writes OUT, the header with this build's version and the release manifest's address (src/engine/platform/update_check.cpp,
# docs/updates.md). Run at every build, so the environment's PT_VERSION (tools/ci/release.py --version) takes effect without a
# reconfigure; the header is rewritten only when its text changes.
if(DEFINED ENV{PT_VERSION} AND NOT "$ENV{PT_VERSION}" STREQUAL "")
  set(PT_VERSION "$ENV{PT_VERSION}")
endif()
if(NOT PT_VERSION MATCHES "^[0-9]+([.][0-9]+)*(-[0-9A-Za-z.]+)?$")
  message(FATAL_ERROR "PT_VERSION '${PT_VERSION}' is not a version like 0.2.0 or 0.2.0-rc1")
endif()
set(text "#pragma once\n#define PT_VERSION \"${PT_VERSION}\"\n#define PT_UPDATE_MANIFEST_URL \"${PT_UPDATE_MANIFEST_URL}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
message(STATUS "pt-port version ${PT_VERSION}")
