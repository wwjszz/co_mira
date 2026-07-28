add_executable(
  co_http_server
  http_server/main.cc
)

find_package(Threads REQUIRED)

add_executable(
  thread_http_server
  thread_http_server/main.cc
)

add_executable(
  callback_http_server
  callback_http_server/main.cc
)

add_executable(
  epoll_http_server
  epoll_http_server/main.cc
)

target_link_libraries(
  thread_http_server
  PRIVATE
    Threads::Threads
)

target_link_libraries(
  callback_http_server
  PRIVATE
    uring
    Threads::Threads
)

target_link_libraries(
  epoll_http_server
  PRIVATE
    Threads::Threads
)

target_link_libraries(
  co_http_server
  PRIVATE
    co_mira
)

target_compile_definitions(
  co_http_server
  PRIVATE
    CO_MIRA_HTTP_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/http_server/www"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    co_http_server
    PRIVATE
      -Wall
      -Wextra
  )

  target_compile_options(
    thread_http_server
    PRIVATE
      -Wall
      -Wextra
  )

  target_compile_options(
    callback_http_server
    PRIVATE
      -Wall
      -Wextra
  )

  target_compile_options(
    epoll_http_server
    PRIVATE
      -Wall
      -Wextra
  )
endif()

find_program(CO_MIRA_CURL_EXECUTABLE curl)
if(BUILD_TESTING AND CO_MIRA_CURL_EXECUTABLE)
  add_test(
    NAME test_http_server
    COMMAND
      sh
      "${PROJECT_SOURCE_DIR}/test/test_http_server.sh"
      "$<TARGET_FILE:co_http_server>"
      "${CMAKE_CURRENT_SOURCE_DIR}/http_server/www"
      "${CO_MIRA_CURL_EXECUTABLE}"
  )

  add_test(
    NAME test_thread_http_server
    COMMAND
      sh
      "${PROJECT_SOURCE_DIR}/test/test_thread_http_server.sh"
      "$<TARGET_FILE:thread_http_server>"
      "${CO_MIRA_CURL_EXECUTABLE}"
  )

  add_test(
    NAME test_callback_http_server
    COMMAND
      sh
      "${PROJECT_SOURCE_DIR}/test/test_thread_http_server.sh"
      "$<TARGET_FILE:callback_http_server>"
      "${CO_MIRA_CURL_EXECUTABLE}"
  )

  add_test(
    NAME test_epoll_http_server
    COMMAND
      sh
      "${PROJECT_SOURCE_DIR}/test/test_thread_http_server.sh"
      "$<TARGET_FILE:epoll_http_server>"
      "${CO_MIRA_CURL_EXECUTABLE}"
  )

  set_tests_properties(
    test_http_server
    test_thread_http_server
    test_callback_http_server
    test_epoll_http_server
    PROPERTIES
      TIMEOUT 20
  )
endif()
