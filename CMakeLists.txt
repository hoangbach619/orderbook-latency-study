cmake_minimum_required(VERSION 3.20)

project(
    orderbook_latency_study
    VERSION 0.1.0
    DESCRIPTION "Comparative latency study of order book price level data structures"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # consumed by clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# Tuning for the measurement build. The reported numbers depend on this and, just as
# much, on disabling frequency scaling and turbo on the measurement machine so that the
# time stamp counter rate matches the core rate. Off by default nowhere except that the
# native tuning can be turned off for portability checks.
option(OBLS_NATIVE_TUNING "Build the benchmark with -O3 -march=native" ON)

# Header only core. The two include roots resolve the obls/ and bench/ prefixes used
# throughout the sources.
add_library(obls_lib INTERFACE)
target_include_directories(obls_lib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                              "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(obls_lib INTERFACE cxx_std_20)

# Benchmark binary. Not run in continuous integration; see .github/workflows/ci.yml.
add_executable(obls_bench bench/benchmark_main.cpp)
target_link_libraries(obls_bench PRIVATE obls_lib)
obls_set_warnings(obls_bench)
obls_enable_sanitizers(obls_bench)
if(OBLS_NATIVE_TUNING)
    # -march=native is the x86 spelling and is what the measurement machine uses. Apple
    # Silicon and other arm targets reject it and want -mcpu=native instead, so detect
    # which one the compiler accepts rather than hard coding a flag that breaks the build
    # elsewhere.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" OBLS_HAS_MARCH_NATIVE)
    if(OBLS_HAS_MARCH_NATIVE)
        set(obls_native_flag -march=native)
    else()
        check_cxx_compiler_flag("-mcpu=native" OBLS_HAS_MCPU_NATIVE)
        if(OBLS_HAS_MCPU_NATIVE)
            set(obls_native_flag -mcpu=native)
        endif()
    endif()
    target_compile_options(obls_bench PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:${obls_native_flag}>)
endif()

# Tests via Catch2, fetched at configure time.
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.4)
FetchContent_MakeAvailable(Catch2)

# Strict warnings as errors must not apply to Catch2's own headers, so mark its include
# directories as system.
foreach(catch_target Catch2 Catch2WithMain)
    if(TARGET ${catch_target})
        get_target_property(catch_inc ${catch_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(catch_inc)
            set_target_properties(${catch_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${catch_inc}")
        endif()
    endif()
endforeach()

enable_testing()
include(CTest)

add_executable(obls_tests
    tests/test_clock.cpp
    tests/test_book_equivalence.cpp
    tests/test_against_lobster.cpp)
target_link_libraries(obls_tests PRIVATE obls_lib Catch2::Catch2WithMain)
target_compile_definitions(obls_tests PRIVATE
    OBLS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
obls_set_warnings(obls_tests)
obls_enable_sanitizers(obls_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
# Catch2 returns exit code 4 for a test that skips itself, which the LOBSTER validation
# does when no sample data is present. Map that to a CTest skip rather than a failure so
# the suite passes without data.
catch_discover_tests(obls_tests PROPERTIES SKIP_RETURN_CODE 4)
