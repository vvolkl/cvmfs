if (BUILD_UNITTESTS OR BUILD_UNITTESTS_DEBUG)
  add_subdirectory (unittests)
endif ()

if (BUILD_INTEGRATION_TESTS)
  add_subdirectory (integration)
endif ()