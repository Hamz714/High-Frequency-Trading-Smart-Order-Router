cmake_minimum_required(VERSION 3.16)
project(HIGH_FREQUENCY LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/bench/bench_lob.cpp
)

add_library(hft_core STATIC ${CORE_SOURCES})
target_include_directories(hft_core PUBLIC include)

# Main app: SOR-vs-naive Monte Carlo comparison + tick-to-trade latency harness
add_executable(high_frequency src/main.cpp)
target_link_libraries(high_frequency PRIVATE hft_core)

# Standalone single-threaded LimitOrderBook microbenchmark
add_executable(bench_lob src/bench/bench_lob.cpp)
target_link_libraries(bench_lob PRIVATE hft_core)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
