cmake_minimum_required(VERSION 3.18)

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

# Gurobi (C API) for -e, found through $GUROBI_HOME as the assignment requires: the header in
# $GUROBI_HOME/include and the newest libgurobiNNN.so in $GUROBI_HOME/lib (no hard-coded version).
# Without it mlrcs still builds; -e then stops with an error message.
set(GUROBI_HOME "$ENV{GUROBI_HOME}")
set(GUROBI_LIBRARY "")
if(GUROBI_HOME AND EXISTS "${GUROBI_HOME}/include/gurobi_c.h")
    file(GLOB GUROBI_CANDIDATES "${GUROBI_HOME}/lib/libgurobi*.so")
    list(FILTER GUROBI_CANDIDATES INCLUDE REGEX "/libgurobi[0-9]+\\.so$")
    if(GUROBI_CANDIDATES)
        list(SORT GUROBI_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET GUROBI_CANDIDATES 0 GUROBI_LIBRARY)
    endif()
endif()
if(GUROBI_LIBRARY)
    message(STATUS "Gurobi: ${GUROBI_LIBRARY}")
    target_include_directories(mlrcs SYSTEM PRIVATE "${GUROBI_HOME}/include")
    # Linked by full path; CMake puts its directory in the build tree's RPATH, so build/mlrcs
    # runs without LD_LIBRARY_PATH
    target_link_libraries(mlrcs PRIVATE "${GUROBI_LIBRARY}")
    target_compile_definitions(mlrcs PRIVATE MLRCS_HAVE_GUROBI=1)
else()
    message(WARNING "Gurobi not found (set GUROBI_HOME): building without -e")
endif()
find_package(Threads REQUIRED)
target_link_libraries(mlrcs PRIVATE Threads::Threads)

option(MLRCS_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
if(MLRCS_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mlrcs PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(mlrcs PRIVATE -fsanitize=address,undefined)
endif()
