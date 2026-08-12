add_library(alcami)
add_library(Alcami::alcami ALIAS alcami)

target_sources(alcami PRIVATE alcami.cpp)

target_include_directories(alcami PUBLIC $<INSTALL_INTERFACE:include> $<BUILD_INTERFACE:${alcami_SOURCE_DIR}/include>
                                         $<BUILD_INTERFACE:${alcami_BINARY_DIR}/include>)

target_link_libraries(alcami PUBLIC Boost::boost fmt::fmt spdlog::spdlog TBB::tbb $<BUILD_INTERFACE:Microsoft.GSL::GSL> $<BUILD_INTERFACE:gtl>)

alcami_target_configure_all(alcami)

if(ALCAMI_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS alcami EXPORT alcami-targets)

  configure_package_config_file("${alcami_SOURCE_DIR}/cmake/AlcamiConfig.cmake.in"
                                "${alcami_BINARY_DIR}/AlcamiConfig.cmake"
                                INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Alcami")

  # NOTE: change compatibility type once we go from alpha to beta, and eventually to release
  write_basic_package_version_file("${alcami_BINARY_DIR}/AlcamiConfigVersion.cmake" VERSION ${PROJECT_VERSION}
                                   COMPATIBILITY ExactVersion)

  install(EXPORT alcami-targets
          FILE AlcamiTargets.cmake
          NAMESPACE Alcami::
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Alcami")

  install(FILES "${alcami_BINARY_DIR}/AlcamiConfig.cmake" "${alcami_BINARY_DIR}/AlcamiConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Alcami")

  install(DIRECTORY "${alcami_SOURCE_DIR}/include/alcami" TYPE INCLUDE)
  install(DIRECTORY "${alcami_BINARY_DIR}/include/alcami" TYPE INCLUDE)
endif()
