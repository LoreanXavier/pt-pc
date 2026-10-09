function(pt_add_staged_file out_var source output)
  get_filename_component(output_dir "${output}" DIRECTORY)
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${source}" "${output}"
    DEPENDS "${source}" VERBATIM)
  set(outputs "${${out_var}}")
  list(APPEND outputs "${output}")
  set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()
