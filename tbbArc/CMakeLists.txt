cmake_minimum_required(VERSION 3.24)

project(cache_bench_tbb LANGUAGES CXX)

include(FetchContent)
find_package(Threads REQUIRED)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug
    Release
    RelWithDebInfo
    MinSizeRel
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "C++ Release flags" FORCE)
else()
  message(WARNING "Untested compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Dependency options
set(TBB_TEST OFF CACHE BOOL "" FORCE)

set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  oneTBB
  GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
  GIT_TAG v2022.3.0
)

FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG v3.3.1
)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.17.0
)

FetchContent_MakeAvailable(oneTBB cxxopts spdlog)

add_executable(cache_bench_tbb
  main.cpp
)

target_compile_features(cache_bench_tbb PRIVATE cxx_std_17)

target_compile_definitions(cache_bench_tbb PRIVATE
  TBB_PREVIEW_CONCURRENT_LRU_CACHE=1
)

target_link_libraries(cache_bench_tbb PRIVATE
  TBB::tbb
  cxxopts::cxxopts
  spdlog::spdlog_header_only
  Threads::Threads
)
