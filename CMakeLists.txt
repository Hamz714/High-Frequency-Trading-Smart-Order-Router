cmake_minimum_required(VERSION 3.16)
project(HIGH_FREQUENCY LANGUAGES CXX)

# Use modern C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directory (your headers)
include_directories(include)

# Collect all .cpp files recursively
file(GLOB_RECURSE SOURCES
    src/*.cpp
)

# Create the executable
add_executable(high_frequency ${SOURCES})
