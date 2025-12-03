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

# Headers are relative to the project root, so we add the co subdirectory.
target_include_directories(co PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/co
)

# Link against Abseil dependencies
target_link_libraries(co PUBLIC
    absl::base
    absl::synchronization # Used by mutex, condition, etc.
    absl::time # Used by channel, future (for timeouts)
)

# Note: If there are tests in co, they would need a separate target
# and link against gtest_main.


