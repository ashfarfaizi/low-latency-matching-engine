cmake_minimum_required(VERSION 3.20)
project(low_latency_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# -march=native tunes benchmarks for the build machine. CI turns it off: the
# runner CPU varies, and some (e.g. AVX10 parts) make clang 18 warn.
option(LOB_NATIVE "Optimise benchmarks/server for this CPU (-march=native)" ON)
set(LOB_FAST_FLAGS -O3)
if(LOB_NATIVE)
  list(APPEND LOB_FAST_FLAGS -march=native)
endif()

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic)

# ── Tests ──────────────────────────────────────────────────────────────────
set(LOB_TEST_TARGETS lob_tests lob_engine_tests lob_wire_tests lob_replay_tests lob_wasm_api_tests)
add_executable(lob_tests tests/test_order_book.cpp)
add_executable(lob_engine_tests tests/test_engine.cpp)
add_executable(lob_wire_tests tests/test_wire.cpp)
add_executable(lob_replay_tests tests/test_replay.cpp)
add_executable(lob_wasm_api_tests tests/test_wasm_api.cpp wasm/lob_wasm.cpp)
foreach(t ${LOB_TEST_TARGETS})
  target_link_libraries(${t} PRIVATE lob Threads::Threads)
endforeach()

# ── Benchmarks ─────────────────────────────────────────────────────────────
add_executable(lob_bench bench/bench_order_book.cpp)
add_executable(lob_bench_engine bench/bench_engine.cpp)
add_executable(lob_bench_spsc bench/bench_spsc.cpp)
foreach(t lob_bench lob_bench_engine lob_bench_spsc)
  target_link_libraries(${t} PRIVATE lob Threads::Threads)
  target_compile_options(${t} PRIVATE ${LOB_FAST_FLAGS})
endforeach()

# ── Server and tools ───────────────────────────────────────────────────────
add_executable(lob_server src/server.cpp)
target_compile_options(lob_server PRIVATE ${LOB_FAST_FLAGS})
add_executable(lob_client src/client.cpp)
add_executable(lob_md_listen src/md_listener.cpp)
add_executable(lob_replay src/replay.cpp)
target_compile_options(lob_replay PRIVATE ${LOB_FAST_FLAGS})
foreach(t lob_server lob_client lob_md_listen lob_replay)
  target_link_libraries(${t} PRIVATE lob Threads::Threads)
endforeach()

# ── Sanitizers ─────────────────────────────────────────────────────────────
option(LOB_SANITIZE "Build tests with ASan + UBSan" OFF)
if(LOB_SANITIZE)
  foreach(t ${LOB_TEST_TARGETS})
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

option(LOB_TSAN "Build engine tests with ThreadSanitizer" OFF)
if(LOB_TSAN)
  foreach(t lob_engine_tests lob_replay_tests)
    target_compile_options(${t} PRIVATE -fsanitize=thread -O1 -g)
    target_link_options(${t} PRIVATE -fsanitize=thread)
  endforeach()
endif()

enable_testing()
foreach(t ${LOB_TEST_TARGETS})
  add_test(NAME ${t} COMMAND ${t})
endforeach()
