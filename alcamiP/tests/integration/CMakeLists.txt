add_executable(alcami-integration-tests)
target_sources(alcami-integration-tests PRIVATE generic_cache_test.cpp)
target_link_libraries(alcami-integration-tests PRIVATE Alcami::alcami gtest_main gmock_main)

alcami_target_configure_compiler(alcami-integration-tests)
alcami_target_configure_cppcheck(alcami-integration-tests)
alcami_target_configure_clangtidy(alcami-integration-tests)
alcami_target_configure_sanitizers(alcami-integration-tests)
alcami_target_configure_warnings(alcami-integration-tests)
alcami_target_configure_coverage(alcami-integration-tests)

gtest_discover_tests(alcami-integration-tests TEST_PREFIX "intg:")
