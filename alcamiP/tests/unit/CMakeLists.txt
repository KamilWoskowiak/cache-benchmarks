add_executable(alcami-unit-tests)
target_sources(alcami-unit-tests PRIVATE lru_test.cpp lfu_test.cpp predefined_policy_test.cpp cache_handle_test.cpp single_thread_manager_test.cpp concurrent_manager_test.cpp)
target_link_libraries(alcami-unit-tests PRIVATE Alcami::alcami gtest_main gmock_main)

alcami_target_configure_compiler(alcami-unit-tests)
alcami_target_configure_cppcheck(alcami-unit-tests)
alcami_target_configure_clangtidy(alcami-unit-tests)
alcami_target_configure_sanitizers(alcami-unit-tests)
alcami_target_configure_warnings(alcami-unit-tests)
alcami_target_configure_coverage(alcami-unit-tests)

gtest_discover_tests(alcami-unit-tests TEST_PREFIX "unit:")
