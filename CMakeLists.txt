cmake_minimum_required(VERSION 3.20)
project(low_latency_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic)

add_executable(lob_tests tests/test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob)

add_executable(lob_bench bench/bench_order_book.cpp)
target_link_libraries(lob_bench PRIVATE lob)
target_compile_options(lob_bench PRIVATE -O3 -march=native)

option(LOB_SANITIZE "Build tests with ASan + UBSan" OFF)
if(LOB_SANITIZE)
  target_compile_options(lob_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
  target_link_options(lob_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
