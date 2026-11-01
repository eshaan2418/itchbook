cmake_minimum_required(VERSION 3.25)
project(itchbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ITCHBOOK_NATIVE "Build with -march=native (local benchmarking)" OFF)
set(ITCHBOOK_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread | empty")
option(ITCHBOOK_FUZZ "Build libFuzzer harnesses (clang with libFuzzer runtime)" OFF)
option(ITCHBOOK_BENCH "Build Google Benchmark micro-benchmarks" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wconversion -Wno-sign-conversion)
if(ITCHBOOK_NATIVE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" AND APPLE)
    add_compile_options(-mcpu=native)
  else()
    add_compile_options(-march=native)
  endif()
endif()
if(ITCHBOOK_SANITIZE MATCHES "thread")
  add_compile_definitions(ITCHBOOK_NO_ALLOC_HOOK)
endif()
if(ITCHBOOK_SANITIZE)
  add_compile_options(-fsanitize=${ITCHBOOK_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${ITCHBOOK_SANITIZE})
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# ---- Library ------------------------------------------------------------------------------------
add_library(itchbook src/gz_reader.cpp src/synth.cpp)
target_include_directories(itchbook PUBLIC include)
target_link_libraries(itchbook PUBLIC ZLIB::ZLIB Threads::Threads)

# ---- Tools (hot path built without exceptions/RTTI) ---------------------------------------------
add_executable(itchbook-cli tools/itchbook.cpp tools/alloc_counter.cpp)
set_target_properties(itchbook-cli PROPERTIES OUTPUT_NAME itchbook)
target_compile_options(itchbook-cli PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(itchbook-cli PRIVATE itchbook)

# ---- Dependencies (tests/benchmarks only) -------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(rapidcheck
  GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
  GIT_TAG 2c3c4365aca21ef4e612768fdc95b1ce8b39a651 SYSTEM)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest rapidcheck)
foreach(t gtest gtest_main gmock rapidcheck rapidcheck_gtest)
  if(TARGET ${t})
    get_target_property(_type ${t} TYPE)
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
      target_compile_options(${t} PRIVATE -w)
    endif()
  endif()
endforeach()

enable_testing()
add_executable(itchbook-tests
  tests/test_parser.cpp tests/test_order_map.cpp tests/test_book.cpp
  tests/test_differential.cpp tests/test_property.cpp tests/test_histogram.cpp
  tests/test_chunker.cpp tests/test_spsc.cpp)
target_link_libraries(itchbook-tests PRIVATE itchbook GTest::gtest_main rapidcheck rapidcheck_gtest)
include(GoogleTest)
gtest_discover_tests(itchbook-tests DISCOVERY_TIMEOUT 60)

# Allocation test overrides global operator new, so it lives in its own binary.
add_executable(itchbook-alloc-test tests/test_alloc.cpp tools/alloc_counter.cpp)
target_link_libraries(itchbook-alloc-test PRIVATE itchbook GTest::gtest_main)
gtest_discover_tests(itchbook-alloc-test DISCOVERY_TIMEOUT 60)

if(ITCHBOOK_BENCH AND NOT ITCHBOOK_SANITIZE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  target_compile_options(benchmark PRIVATE -w)
  add_executable(itchbook-bench bench/micro_bench.cpp)
  target_link_libraries(itchbook-bench PRIVATE itchbook benchmark::benchmark)
endif()

if(ITCHBOOK_FUZZ)
  add_executable(fuzz-parser fuzz/fuzz_parser.cpp)
  target_compile_options(fuzz-parser PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz-parser PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz-parser PRIVATE itchbook)
endif()
