cmake_minimum_required(VERSION 3.18)
project(alcami
        VERSION 0.0.1
        DESCRIPTION "A C++20 concurrent caching library for arbitrary caching policies"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${alcami_SOURCE_DIR}/cmake" "${alcami_BINARY_DIR}/cmake")

# This is set to ON when this is the root project (see below).
set(ALCAMI_INSTALL_DEFAULT OFF)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPE)
    message(STATUS "No build type specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()

  set(ALCAMI_INSTALL_DEFAULT ON)

  include(CTest)
endif()

# Generate ProjectUtils cmake file.
set(PROJECT_UTILS_VAR_PREFIX ALCAMI)
set(PROJECT_UTILS_FUNC_PREFIX alcami)
configure_file("${alcami_SOURCE_DIR}/cmake/ProjectUtils.cmake.in" "cmake/AlcamiProjectUtils.cmake" @ONLY)

# Override ProjectUtils defaults.
set(ALCAMI_CLANG_TIDY_OPTIONS "-config-file=${alcami_SOURCE_DIR}/.clang-tidy" CACHE STRING
                                                                                    "List of options for clang-tidy")

# Add ProjectUtils options.
include(AlcamiProjectUtils)

option(ALCAMI_INSTALL "Install this project" ${ALCAMI_INSTALL_DEFAULT})
option(ALCAMI_BUILD_TOOLS "Build with tools" ON)
option(ALCAMI_BUILD_TESTS "Build with tests" OFF)
option(ALCAMI_BUILD_EXAMPLES "Build with examples" OFF)
option(ALCAMI_BUILD_DOXYGEN "Build with doxygen docs" OFF)
option(ALCAMI_INSTALL_MSGSL "Install Microsoft.GSL library with this project" OFF)

configure_file("${alcami_SOURCE_DIR}/config/config.h.in" "include/alcami/config.h")

# Configure and download dependencies.
include(AlcamiBuildDependencies)

add_subdirectory(src)

if(ALCAMI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ALCAMI_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(ALCAMI_BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(ALCAMI_BUILD_DOXYGEN)
  add_subdirectory(doxygen)
endif()
