include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)
include(GoogleTest)

file(GLOB_RECURSE UNIT_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(unit_tests ${UNIT_TEST_SOURCES} ${PROJECT_SOURCES})

target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(unit_tests PRIVATE gtest_main)

gtest_discover_tests(unit_tests)
