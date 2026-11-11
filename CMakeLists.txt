cmake_minimum_required(VERSION 3.16)

project(mlrcs LANGUAGES CXX)

# The grader may run plain `cmake -B build` without a build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

file(GLOB_RECURSE MLRCS_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(mlrcs ${MLRCS_SOURCES})
target_include_directories(mlrcs PRIVATE src)
target_compile_features(mlrcs PRIVATE cxx_std_17)
set_target_properties(mlrcs PROPERTIES
    CXX_EXTENSIONS OFF
    # Put the binary at build/mlrcs, not build/src/mlrcs
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # No -march=native: the grading machine may have a different CPU
    target_compile_options(mlrcs PRIVATE -pipe -Wall -Wextra -Wpedantic -Wshadow)
endif()

option(MLRCS_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(MLRCS_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mlrcs PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mlrcs PRIVATE -fsanitize=address,undefined)
endif()
