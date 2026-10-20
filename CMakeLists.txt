cmake_minimum_required(VERSION 3.16)
project(kvstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KV_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(KV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(kvcore src/resp.cpp src/store.cpp src/aof.cpp src/commands.cpp src/server.cpp)
target_include_directories(kvcore PUBLIC src)

add_executable(kvstore src/main.cpp)
target_link_libraries(kvstore PRIVATE kvcore)

enable_testing()
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(kv_tests
  tests/resp_test.cpp tests/store_test.cpp tests/commands_test.cpp
  tests/aof_test.cpp tests/glob_test.cpp)
target_link_libraries(kv_tests PRIVATE kvcore GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(kv_tests)
