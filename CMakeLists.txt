cmake_minimum_required(VERSION 3.20)

project(co_mira VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(
  CO_MIRA_LIBURING
  REQUIRED
  IMPORTED_TARGET
  liburing
)

# Cross-scheduler wakeup and submission are MSG_RING-only. There is no eventfd
# fallback, so reject unsupported target kernels during configuration.
set(CO_MIRA_MINIMUM_KERNEL_VERSION "5.19")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   CMAKE_SYSTEM_VERSION VERSION_LESS "${CO_MIRA_MINIMUM_KERNEL_VERSION}")
  message(
    FATAL_ERROR
      "co_mira requires Linux ${CO_MIRA_MINIMUM_KERNEL_VERSION}+ with IORING_OP_MSG_RING and async-cancel ANY|ALL support; target system is ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}"
  )
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only co_mira library.
add_library(co_mira INTERFACE)
add_library(co_mira::co_mira ALIAS co_mira)

target_compile_features(
  co_mira
  INTERFACE
    cxx_std_23
)

target_include_directories(
  co_mira
  INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(
  co_mira
  INTERFACE
    PkgConfig::CO_MIRA_LIBURING
)

# Enable internal counters only in Debug builds.
target_compile_definitions(
  co_mira
  INTERFACE
    $<$<CONFIG:Debug>:CO_MIRA_ENABLE_COUNTERS>
)

set(TEST_BIN_DIR
    "${PROJECT_BINARY_DIR}/test/bin")

file(MAKE_DIRECTORY
     "${TEST_BIN_DIR}")

file(
  GLOB TEST_SOURCES
  CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/test/*.cc"
    "${PROJECT_SOURCE_DIR}/test/*.cpp"
    "${PROJECT_SOURCE_DIR}/test/*.cxx"
)

foreach(TEST_SOURCE IN LISTS TEST_SOURCES)
  get_filename_component(
    TEST_NAME
    "${TEST_SOURCE}"
    NAME_WE
  )

  add_executable(
    "${TEST_NAME}"
    "${TEST_SOURCE}"
  )

  if(BUILD_TESTING AND TEST_NAME MATCHES "^test_")
    target_compile_definitions(
      "${TEST_NAME}"
      PRIVATE
        CO_MIRA_ENABLE_TESTING
    )

    add_test(
      NAME "${TEST_NAME}"
      COMMAND "$<TARGET_FILE:${TEST_NAME}>"
    )

    set_tests_properties(
      "${TEST_NAME}"
      PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
  endif()

  target_link_libraries(
    "${TEST_NAME}"
    PRIVATE
      co_mira
  )

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      "${TEST_NAME}"
      PRIVATE
        -Wall
        -Wextra
    )
  endif()

  set_target_properties(
    "${TEST_NAME}"
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY
        "${TEST_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG
        "${TEST_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE
        "${TEST_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO
        "${TEST_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL
        "${TEST_BIN_DIR}"
  )
endforeach()

if(BUILD_TESTING)
  add_test(
    NAME test_install_consumer
    COMMAND
      "${CMAKE_COMMAND}"
      "-DCO_MIRA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DCO_MIRA_BINARY_DIR=${PROJECT_BINARY_DIR}"
      "-DCO_MIRA_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DCO_MIRA_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCO_MIRA_CONFIG=$<CONFIG>"
      "-DCO_MIRA_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
      -P
      "${PROJECT_SOURCE_DIR}/test/install_consumer.cmake"
  )

  set_tests_properties(
    test_install_consumer
    PROPERTIES
      RUN_SERIAL TRUE
      TIMEOUT 120
  )
endif()

include(CMakePackageConfigHelpers)

set(
  CO_MIRA_CMAKE_INSTALL_DIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/co_mira"
)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/cmake")

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/co_miraConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/cmake/co_miraConfig.cmake"
  INSTALL_DESTINATION
    "${CO_MIRA_CMAKE_INSTALL_DIR}"
)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/cmake/co_miraConfigVersion.cmake"
  VERSION
    "${PROJECT_VERSION}"
  COMPATIBILITY
    SameMajorVersion
)

install(
  TARGETS
    co_mira
  EXPORT
    co_miraTargets
  INCLUDES DESTINATION
    "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY
    "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION
    "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  EXPORT
    co_miraTargets
  FILE
    co_miraTargets.cmake
  NAMESPACE
    co_mira::
  DESTINATION
    "${CO_MIRA_CMAKE_INSTALL_DIR}"
)

install(
  FILES
    "${PROJECT_BINARY_DIR}/cmake/co_miraConfig.cmake"
    "${PROJECT_BINARY_DIR}/cmake/co_miraConfigVersion.cmake"
  DESTINATION
    "${CO_MIRA_CMAKE_INSTALL_DIR}"
)
