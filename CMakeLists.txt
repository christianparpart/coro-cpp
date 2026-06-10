# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(COROUTINE_DEMO_VERSION COROUTINE_DEMO_VERSION_STRING)

project(coroutine-demo
    VERSION "${COROUTINE_DEMO_VERSION}"
    DESCRIPTION "Lightning-talk demos for C++23 coroutines (Task<T>, scheduler, awaiters)"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# ----------------------------------------------------------------------------
# Project-wide options

option(COROUTINE_DEMO_BUILD_TESTS "Build unit tests" ON)
option(COROUTINE_DEMO_BUILD_DEMOS "Build the lightning-talk demo executables" ON)
option(USE_SCCACHE "Use sccache as the compiler launcher when the binary is on PATH" ON)

# ----------------------------------------------------------------------------
# Compiler caching (sccache). Loaded before any compiler invocation so that
# CPM-fetched dependencies also get cached. cache.cmake silently no-ops when
# sccache is not installed or USE_SCCACHE=OFF.

include(cache)

# Default install/runtime/library output directories.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()

# ----------------------------------------------------------------------------
# Dependencies via CPM (tries find_package first).
#
# Fetched BEFORE the pedantic/sanitizer/clang-tidy includes so the global
# add_compile_options() / CMAKE_CXX_CLANG_TIDY they install only apply to our
# own targets, not to third-party code where -Werror is not our problem.

set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Prefer system-installed packages over CPM downloads")
include(cmake/CPM.cmake)

if(COROUTINE_DEMO_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.6.0
    )

    # Locate the directory shipping Catch.cmake (providing catch_discover_tests)
    # and put it on CMAKE_MODULE_PATH so test subdirectories can simply
    # include(Catch).
    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    elseif(Catch2_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    endif()
endif()

# ----------------------------------------------------------------------------
# Toolchain helpers (warning levels, sanitizers, clang-tidy).

include(PedanticCompiler)
include(Sanitizers)
include(ClangTidy)

# ----------------------------------------------------------------------------
# Test wiring is enabled early so subdirectories can register CTest tests
# via catch_discover_tests().

if(COROUTINE_DEMO_BUILD_TESTS)
    enable_testing()
endif()

# ----------------------------------------------------------------------------
# Library target: the Coro library carries Task<T>, the Scheduler interface,
# the concrete event loop, and the awaiters. Both demos and the test binary
# link against it.

add_library(Coro STATIC)
target_include_directories(Coro PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(Coro PUBLIC cxx_std_23)

add_subdirectory(src/Coro)

# ----------------------------------------------------------------------------
# Demos.

if(COROUTINE_DEMO_BUILD_DEMOS)
    add_subdirectory(src/demos)
endif()

# ----------------------------------------------------------------------------
# Tests.

if(COROUTINE_DEMO_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()
