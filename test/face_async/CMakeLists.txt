# face_async_tests — pure-logic unit tests for the qtifaceinfer async refactor.
# Standalone CMake project (independent from the plugin build); builds and runs
# ONLY on the board (aarch64). Vendored gtest 1.14.0 lives in third_party/.
# No network access at build time.

cmake_minimum_required(VERSION 3.16)
project(face_async_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(face_async_tests
  slot_pool_tests.cpp
  face_mailbox_tests.cpp
  result_store_tests.cpp
  stage_reporter_tests.cpp
  frame_layout_tests.cpp
  mask_policy_tests.cpp
  face_worker_flow_tests.cpp
  face_worker_concurrency_tests.cpp
  third_party/googletest/src/gtest-all.cc
  third_party/googletest/src/gtest_main.cc
)

target_include_directories(face_async_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}              # test_helpers.hpp
  ${REPO_ROOT}/plugins/qtifaceinfer        # async/*.hpp
  ${REPO_ROOT}/include                     # aibox/face/face_mask_policy.hpp
  third_party/googletest/include
  third_party/googletest
)

target_compile_options(face_async_tests PRIVATE
  -Wall -Wextra
  -Wno-unused-parameter
)

find_package(Threads REQUIRED)
target_link_libraries(face_async_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME face_async_tests COMMAND face_async_tests)
