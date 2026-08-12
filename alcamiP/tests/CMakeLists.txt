include(GoogleTest)

add_subdirectory(unit)
add_subdirectory(integration)

if(SECA_ENABLE_COVERAGE)
  find_program(LCOV_PATH lcov REQUIRED)
  find_program(CTEST_PATH ctest REQUIRED)

  setup_target_for_coverage_lcov(NAME alcami-coverage
                                 EXECUTABLE "${CTEST_PATH}"
                                 DEPENDENCIES Alcami::alcami alcami-unit-tests
                                 EXCLUDE "${alcami_BINARY_DIR}/_deps/*" "tests/*" "tools/*"
                                 LCOV_ARGS --no-external NO_DEMANGLE)
endif()
