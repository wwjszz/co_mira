cmake_minimum_required(VERSION 3.20)

project(co_mira LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only co_mira library.
add_library(co_mira INTERFACE)

target_include_directories(
  co_mira
  INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(
  co_mira
  INTERFACE
    uring
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