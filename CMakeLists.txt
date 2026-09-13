cmake_minimum_required(VERSION 3.20)
project(lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_BUILD_TESTS  "Build GoogleTest unit tests"        ON)
option(LOB_BUILD_BENCH  "Build replay benchmark"             ON)
option(LOB_BUILD_PYTHON "Build the pybind11 module"          OFF)
option(LOB_SANITIZE     "Enable ASan/UBSan (Debug builds)"   OFF)
option(LOB_NATIVE       "Tune for the build machine's CPU"   ON)

# scikit-build-core sets SKBUILD; it only wants the Python module.
if(SKBUILD)
  set(LOB_BUILD_TESTS OFF)
  set(LOB_BUILD_BENCH OFF)
  set(LOB_BUILD_PYTHON ON)
endif()

add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

# Apple clang on arm64 rejects -march=native; prefer -mcpu=native there.
include(CheckCXXCompilerFlag)
add_library(lob_tuning INTERFACE)
if(LOB_NATIVE)
  check_cxx_compiler_flag("-march=native" LOB_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag("-mcpu=native"  LOB_HAS_MCPU_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    target_compile_options(lob_tuning INTERFACE $<$<CONFIG:Release>:-march=native>)
  elseif(LOB_HAS_MCPU_NATIVE)
    target_compile_options(lob_tuning INTERFACE $<$<CONFIG:Release>:-mcpu=native>)
  endif()
endif()
if(LOB_SANITIZE)
  target_compile_options(lob_tuning INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_tuning INTERFACE -fsanitize=address,undefined)
endif()

# ---- core library -----------------------------------------------------------
add_library(lob STATIC
  cpp/src/order_book.cpp
  cpp/src/lobster.cpp
  cpp/src/replay.cpp
  cpp/src/fill_model.cpp
  cpp/src/backtester.cpp
  cpp/src/ofi_strategy.cpp
  cpp/src/metrics.cpp
)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
target_link_libraries(lob PUBLIC lob_warnings lob_tuning)
set_target_properties(lob PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- tests --------------------------------------------------------------------
if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(lob_tests
    cpp/tests/price_level_test.cpp
    cpp/tests/order_book_test.cpp
    cpp/tests/lobster_replay_test.cpp
    cpp/tests/fill_model_test.cpp
    cpp/tests/backtester_test.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

# ---- benchmark ------------------------------------------------------------------
if(LOB_BUILD_BENCH)
  add_executable(replay_bench cpp/bench/replay_bench.cpp)
  target_link_libraries(replay_bench PRIVATE lob)
endif()

# ---- python module ----------------------------------------------------------------
if(LOB_BUILD_PYTHON)
  find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_lob MODULE bindings/module.cpp)
  target_link_libraries(_lob PRIVATE lob)
  install(TARGETS _lob DESTINATION lob)
endif()
