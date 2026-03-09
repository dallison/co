#STATIC CMakeLists.txt for the co project
# This file defines the 'co' library and its dependencies using CMake.

cmake_minimum_required(VERSION 3.15)
project(co LANGUAGES CXX ASM) # Add ASM language for .S files

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Prefer standard C++

# Add common compile options (from co/BUILD)
add_compile_options(-Wall)

# --- Apple Silicon (ARM64) specific settings ---
# CMAKE_OSX_ARCHITECTURES is expected to be set by the parent project or environment.
# We will propagate this value to sub-dependencies.
if(APPLE)
    message(STATUS "Building for Apple platform. Propagating CMAKE_OSX_ARCHITECTURES: ${CMAKE_OSX_ARCHITECTURES}")
endif()

# Include FetchContent module to manage Abseil dependency
include(FetchContent)

# --- External Dependency: Abseil ---
# The 'co' project depends on Abseil, so we need to fetch and make it available.
FetchContent_Declare(
    abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20230802.0 # Matches the version in co's WORKSPACE
    # Pass architecture settings to Abseil's CMake build
    CMAKE_ARGS
        CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES}"
)
FetchContent_MakeAvailable(abseil)
# Abseil provides targets like absl::base, absl::strings, etc.

# --- External Dependency: Googletest ---
# Fetches Googletest and integrates its CMake build.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.11.0 # Matches the version in your WORKSPACE
    # Pass architecture settings to Googletest's CMake build
    CMAKE_ARGS
        CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES}"
)
FetchContent_MakeAvailable(googletest)
# Googletest provides targets like gtest_main, gtest, gmock.

# --- co Library Target ---
# Corresponds to //:co in Bazel
add_library(co STATIC
    co/coroutine.cc
    co/coroutine.h
    co/bitset.h
    co/context.S
    co/context.h
    co/detect_sanitizers.h
)

# Headers use "co/coroutine.h" style includes, so we add the project root.
target_include_directories(co PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link against Abseil dependencies
target_link_libraries(co PUBLIC
    absl::base
    absl::synchronization # Used by mutex, condition, etc.
    absl::time # Used by channel, future (for timeouts)
)

# --- co_cpp20 Library Target ---
# Standalone C++20 coroutine library (no dependency on 'co' or Abseil)
add_library(co_cpp20 STATIC
    co/coroutine_cpp20.cc
    co/coroutine_cpp20.h
)

target_include_directories(co_cpp20 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(co_cpp20 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# --- Tests ---
enable_testing()

# C++17 coroutine tests
add_executable(coroutines_test
    co/coroutines_test.cc
)
target_link_libraries(coroutines_test PRIVATE
    co
    absl::status
    gtest_main
)
add_test(NAME coroutines_test COMMAND coroutines_test)

# C++20 coroutine tests
add_executable(test_cpp20
    co/test_cpp20.cc
)
target_link_libraries(test_cpp20 PRIVATE
    co_cpp20
    gtest_main
)
set_target_properties(test_cpp20 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME test_cpp20 COMMAND test_cpp20)
