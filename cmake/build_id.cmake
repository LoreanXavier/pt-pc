# Writes OUT with PT_BUILD_ID, the short commit hash of SOURCE ("-dirty" with local changes to tracked files, "dev" without git).
# Run at every build; the header is rewritten only when the id changes, so an unchanged tree does not rebuild main.cpp.
execute_process(COMMAND git -C "${SOURCE}" rev-parse --short=7 HEAD OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                RESULT_VARIABLE failed)
if(failed OR id STREQUAL "")
  set(id "dev")
else()
  execute_process(COMMAND git -C "${SOURCE}" diff --quiet HEAD -- RESULT_VARIABLE dirty ERROR_QUIET)
  if(dirty)
    set(id "${id}-dirty")
  endif()
endif()
set(text "#pragma once\n#define PT_BUILD_ID \"${id}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
