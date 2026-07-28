cmake_minimum_required(VERSION 3.20)

project(co_mira_consumer LANGUAGES CXX)

find_package(co_mira CONFIG REQUIRED)

add_executable(
  co_mira_consumer
  main.cc
)

target_link_libraries(
  co_mira_consumer
  PRIVATE
    co_mira::co_mira
)

enable_testing()
add_test(
  NAME co_mira_consumer
  COMMAND co_mira_consumer
)
