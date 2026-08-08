cmake_minimum_required(VERSION 3.16)
project(HIGH_FREQUENCY LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE specified; defaulting to Release")
endif()

add_library(hft_warnings INTERFACE)
if(MSVC)
    target_compile_options(hft_warnings INTERFACE /W4)
else()
    target_compile_options(hft_warnings INTERFACE -Wall -Wextra)
endif()

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/bench/bench_lob.cpp
    ${CMAKE_SOURCE_DIR}/src/bench/bench_sor.cpp
    ${CMAKE_SOURCE_DIR}/src/calibrate/main.cpp
)

add_library(hft_core STATIC ${CORE_SOURCES})
target_include_directories(hft_core PUBLIC include)
target_link_libraries(hft_core PRIVATE hft_warnings)

# Main app: SOR-vs-naive Monte Carlo comparison + tick-to-trade latency harness
add_executable(high_frequency src/main.cpp)
target_link_libraries(high_frequency PRIVATE hft_core hft_warnings)

# Standalone single-threaded LimitOrderBook microbenchmark
add_executable(bench_lob src/bench/bench_lob.cpp)
target_link_libraries(bench_lob PRIVATE hft_core hft_warnings)

# Standalone single-threaded routing-decision (DPEngine) microbenchmark
add_executable(bench_sor src/bench/bench_sor.cpp)
target_link_libraries(bench_sor PRIVATE hft_core hft_warnings)

# Randomized parameter-sweep calibration tool
add_executable(calibrate src/calibrate/main.cpp)
target_link_libraries(calibrate PRIVATE hft_core hft_warnings)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
