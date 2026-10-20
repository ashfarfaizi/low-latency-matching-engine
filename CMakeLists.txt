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

find_package(Threads REQUIRED)

add_executable(lob_engine_tests tests/test_engine.cpp)
target_link_libraries(lob_engine_tests PRIVATE lob Threads::Threads)

add_executable(lob_bench bench/bench_order_book.cpp)
target_link_libraries(lob_bench PRIVATE lob)
target_compile_options(lob_bench PRIVATE -O3 -march=native)

add_executable(lob_bench_engine bench/bench_engine.cpp)
target_link_libraries(lob_bench_engine PRIVATE lob Threads::Threads)
target_compile_options(lob_bench_engine PRIVATE -O3 -march=native)

option(LOB_SANITIZE "Build tests with ASan + UBSan" OFF)
if(LOB_SANITIZE)
  foreach(t lob_tests lob_engine_tests)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

option(LOB_TSAN "Build engine tests with ThreadSanitizer" OFF)
if(LOB_TSAN)
  target_compile_options(lob_engine_tests PRIVATE -fsanitize=thread -O1 -g)
  target_link_options(lob_engine_tests PRIVATE -fsanitize=thread)
endif()

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
add_test(NAME lob_engine_tests COMMAND lob_engine_tests)

add_executable(lob_bench_spsc bench/bench_spsc.cpp)
target_link_libraries(lob_bench_spsc PRIVATE lob Threads::Threads)
target_compile_options(lob_bench_spsc PRIVATE -O3 -march=native)
