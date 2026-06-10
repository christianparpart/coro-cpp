# SPDX-License-Identifier: Apache-2.0
# Compiler caching via sccache. No-op when sccache is not on PATH or
# USE_SCCACHE is OFF.

find_program(SCCACHE sccache)

if(SCCACHE AND USE_SCCACHE)
    message(STATUS "Enabling sccache")
    set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
else()
    message(STATUS "sccache not found or disabled by USE_SCCACHE option, caching disabled")
endif()
