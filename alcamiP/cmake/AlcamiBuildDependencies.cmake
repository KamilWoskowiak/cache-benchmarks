find_package(Boost 1.74 CONFIG REQUIRED)
find_package(fmt 7.1.3 REQUIRED)
find_package(spdlog 1.8.1 REQUIRED)
find_package(TBB REQUIRED)

include(FetchContent)

# Append dependencies to make available with FetchContent.
set(FETCH_DEPS "")

# Append dependencies' cache variables to hide in UI.
set(ADVANCED_OPTS "")

# Microsoft.GSL
list(APPEND FETCH_DEPS MicrosoftGSL)
list(APPEND ADVANCED_OPTS GSL_CXX_STANDARD GSL_INSTALL GSL_TEST)
FetchContent_Declare(MicrosoftGSL
                     GIT_REPOSITORY "https://github.com/microsoft/GSL"
                     GIT_TAG "v4.0.0"
                     GIT_SHALLOW ON)

if(ALCAMI_INSTALL_MSGSL)
  set(GSL_INSTALL ON CACHE INTERNAL "")
endif()

# cxxopts
list(APPEND FETCH_DEPS cxxopts)
list(APPEND
     ADVANCED_OPTS
     CXXOPTS_BUILD_EXAMPLES
     CXXOPTS_BUILD_TESTS
     CXXOPTS_ENABLE_INSTALL
     CXXOPTS_ENABLE_WARNINGS
     CXXOPTS_USE_UNICODE_HELP)

FetchContent_Declare(cxxopts
                     GIT_REPOSITORY "https://github.com/jarro2783/cxxopts"
                     GIT_TAG "v3.2.0"
                     GIT_SHALLOW ON)

# GTL / phmap
list(APPEND FETCH_DEPS gtl)
list(APPEND
     ADVANCED_OPTS
     GTL_INSTALL
     GTL_BUILD_TESTS
     GTL_BUILD_EXAMPLES
     GTL_BUILD_BENCHMARKS
     GTL_DOWNLOAD_GTEST)

set(GTL_BUILD_TESTS OFF CACHE INTERNAL "")
set(GTL_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(GTL_BUILD_BENCHMARKS OFF CACHE INTERNAL "")
set(GTL_DOWNLOAD_GTEST OFF CACHE INTERNAL "")

if(ALCAMI_INSTALL_GTL)
  set(GTL_INSTALL ON CACHE INTERNAL "")
else()
  set(GTL_INSTALL OFF CACHE INTERNAL "")
endif()

FetchContent_Declare(gtl
                     GIT_REPOSITORY "https://github.com/greg7mdp/gtl.git"
                     GIT_TAG "v1.2.0"
                     GIT_SHALLOW ON)

# GoogleTest
if(ALCAMI_BUILD_TESTS AND BUILD_TESTING)
  set(INSTALL_GTEST OFF CACHE INTERNAL "")

  list(APPEND FETCH_DEPS googletest)
  list(APPEND ADVANCED_OPTS BUILD_GMOCK GTEST_HAS_ABSL)

  FetchContent_Declare(googletest
                       GIT_REPOSITORY "https://github.com/google/googletest/"
                       GIT_TAG "v1.15.2"
                       GIT_SHALLOW ON)
endif()

# Doxygen Awesome
if(ALCAMI_BUILD_DOXYGEN)
  list(APPEND FETCH_DEPS doxygen_awesome)

  FetchContent_Declare(doxygen_awesome
                       GIT_REPOSITORY "https://github.com/jothepro/doxygen-awesome-css.git"
                       GIT_TAG "v2.3.4"
                       GIT_SHALLOW ON)
endif()

# Build FetchContent dependencies.
FetchContent_MakeAvailable(${FETCH_DEPS})

mark_as_advanced(FORCE ${ADVANCED_OPTS})
