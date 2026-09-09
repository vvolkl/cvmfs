# Wave 2: register each shell integration test as an individual CTest test
# while keeping test/run.sh as the execution engine.

set (_cvmfs_integration_test_root "${CMAKE_CURRENT_LIST_DIR}/..")
set (_cvmfs_integration_launcher "${CMAKE_CURRENT_LIST_DIR}/run_ctest.sh")

function (cvmfs_integration_extract_metadata MAIN_FILE VARIABLE OUTPUT_VARIABLE)
  file (READ "${MAIN_FILE}" _main_contents)
  set (_value "")

  if (_main_contents MATCHES "(^|\n)[ \t]*${VARIABLE}=\"([^\"]*)\"")
    set (_value "${CMAKE_MATCH_2}")
  elseif (_main_contents MATCHES "(^|\n)[ \t]*${VARIABLE}=([0-9]+)")
    set (_value "${CMAKE_MATCH_2}")
  elseif (_main_contents MATCHES "(^|\n)[ \t]*${VARIABLE}=([^ \t\r\n#]+)")
    set (_value "${CMAKE_MATCH_2}")
  endif ()

  set (${OUTPUT_VARIABLE} "${_value}" PARENT_SCOPE)
endfunction ()

file (GLOB _cvmfs_integration_test_mains
      RELATIVE "${_cvmfs_integration_test_root}"
      "${_cvmfs_integration_test_root}/src/*/main")
list (SORT _cvmfs_integration_test_mains)

foreach (_main_rel IN LISTS _cvmfs_integration_test_mains)
  get_filename_component (_test_dir_rel "${_main_rel}" DIRECTORY)
  get_filename_component (_test_id      "${_test_dir_rel}" NAME)
  set (_main_file "${_cvmfs_integration_test_root}/${_main_rel}")

  cvmfs_integration_extract_metadata ("${_main_file}" cvmfs_test_suites  _test_suites)
  cvmfs_integration_extract_metadata ("${_main_file}" cvmfs_test_timeout _test_timeout)

  set (_test_labels)
  if (NOT _test_suites STREQUAL "")
    separate_arguments (_test_labels UNIX_COMMAND "${_test_suites}")
  endif ()

  if (_test_timeout STREQUAL "")
    list (FIND _test_labels quick _quick_label_idx)
    if (NOT _quick_label_idx EQUAL -1)
      set (_test_timeout 300)
    endif ()
  endif ()

  set (_test_output_dir "${CMAKE_CURRENT_BINARY_DIR}/runs/${_test_id}")
  set (_ctest_name "integration.${_test_id}")

  add_test (
    NAME    "${_ctest_name}"
    COMMAND bash "${_cvmfs_integration_launcher}"
            "${_cvmfs_integration_test_root}"
            "${_test_dir_rel}"
            "${_test_output_dir}"
  )

  set_tests_properties ("${_ctest_name}" PROPERTIES RUN_SERIAL TRUE)

  if (_test_labels)
    set_tests_properties ("${_ctest_name}" PROPERTIES LABELS "${_test_labels}")
  endif ()

  if (NOT _test_timeout STREQUAL "" AND NOT _test_timeout STREQUAL "0")
    set_tests_properties ("${_ctest_name}" PROPERTIES TIMEOUT "${_test_timeout}")
  endif ()
endforeach ()

list (LENGTH _cvmfs_integration_test_mains _cvmfs_integration_test_count)
message (STATUS "Registered ${_cvmfs_integration_test_count} shell integration tests with CTest")