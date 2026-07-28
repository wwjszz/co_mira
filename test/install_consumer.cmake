cmake_minimum_required(VERSION 3.20)

foreach(
  required_variable
  IN ITEMS
    CO_MIRA_SOURCE_DIR
    CO_MIRA_BINARY_DIR
    CO_MIRA_INSTALL_LIBDIR
    CO_MIRA_CTEST_COMMAND
)
  if(NOT DEFINED "${required_variable}")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(install_prefix "${CO_MIRA_BINARY_DIR}/test/install-prefix")
set(consumer_build_dir "${CO_MIRA_BINARY_DIR}/test/consumer-build")

file(
  REMOVE_RECURSE
  "${install_prefix}"
  "${consumer_build_dir}"
)

set(
  install_command
  "${CMAKE_COMMAND}"
  --install
  "${CO_MIRA_BINARY_DIR}"
  --prefix
  "${install_prefix}"
)

if(DEFINED CO_MIRA_CONFIG AND NOT CO_MIRA_CONFIG STREQUAL "")
  list(
    APPEND
    install_command
    --config
    "${CO_MIRA_CONFIG}"
  )
endif()

execute_process(
  COMMAND
    ${install_command}
  RESULT_VARIABLE
    install_result
  OUTPUT_VARIABLE
    install_output
  ERROR_VARIABLE
    install_error
)

if(NOT install_result EQUAL 0)
  message(
    FATAL_ERROR
      "co_mira installation failed\n${install_output}\n${install_error}"
  )
endif()

set(
  configure_command
  "${CMAKE_COMMAND}"
  -S
  "${CO_MIRA_SOURCE_DIR}/test/consumer"
  -B
  "${consumer_build_dir}"
  "-Dco_mira_DIR=${install_prefix}/${CO_MIRA_INSTALL_LIBDIR}/cmake/co_mira"
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
)

if(DEFINED CO_MIRA_BUILD_TYPE AND NOT CO_MIRA_BUILD_TYPE STREQUAL "")
  list(
    APPEND
    configure_command
    "-DCMAKE_BUILD_TYPE=${CO_MIRA_BUILD_TYPE}"
  )
endif()

execute_process(
  COMMAND
    ${configure_command}
  RESULT_VARIABLE
    configure_result
  OUTPUT_VARIABLE
    configure_output
  ERROR_VARIABLE
    configure_error
)

if(NOT configure_result EQUAL 0)
  message(
    FATAL_ERROR
      "consumer configuration failed\n${configure_output}\n${configure_error}"
  )
endif()

set(
  build_command
  "${CMAKE_COMMAND}"
  --build
  "${consumer_build_dir}"
)

if(DEFINED CO_MIRA_CONFIG AND NOT CO_MIRA_CONFIG STREQUAL "")
  list(
    APPEND
    build_command
    --config
    "${CO_MIRA_CONFIG}"
  )
endif()

execute_process(
  COMMAND
    ${build_command}
  RESULT_VARIABLE
    build_result
  OUTPUT_VARIABLE
    build_output
  ERROR_VARIABLE
    build_error
)

if(NOT build_result EQUAL 0)
  message(
    FATAL_ERROR
      "consumer build failed\n${build_output}\n${build_error}"
  )
endif()

set(
  test_command
  "${CO_MIRA_CTEST_COMMAND}"
  --test-dir
  "${consumer_build_dir}"
  --output-on-failure
)

if(DEFINED CO_MIRA_CONFIG AND NOT CO_MIRA_CONFIG STREQUAL "")
  list(
    APPEND
    test_command
    -C
    "${CO_MIRA_CONFIG}"
  )
endif()

execute_process(
  COMMAND
    ${test_command}
  RESULT_VARIABLE
    test_result
  OUTPUT_VARIABLE
    test_output
  ERROR_VARIABLE
    test_error
)

if(NOT test_result EQUAL 0)
  message(
    FATAL_ERROR
      "consumer test failed\n${test_output}\n${test_error}"
  )
endif()
