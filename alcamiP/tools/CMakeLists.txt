add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE Alcami::alcami spdlog::spdlog cxxopts)
alcami_target_configure_all(benchmark)

add_executable(twitter_benchmark twitter_benchmark.cpp)
target_link_libraries(twitter_benchmark PRIVATE Alcami::alcami spdlog::spdlog cxxopts)
alcami_target_configure_all(twitter_benchmark)

add_executable(page_benchmark page_benchmark.cpp)
target_link_libraries(page_benchmark PRIVATE Alcami::alcami spdlog::spdlog cxxopts)
alcami_target_configure_all(page_benchmark)

add_executable(meta_kv_benchmark meta_kv_benchmark.cpp)
target_link_libraries(meta_kv_benchmark PRIVATE Alcami::alcami spdlog::spdlog cxxopts)
alcami_target_configure_all(meta_kv_benchmark)
