# HomebrewLLVM.cmake - macOS toolchain file selecting Homebrew's LLVM clang.
#
# Used as a CMAKE_TOOLCHAIN_FILE (wired via the `clang-*` CMake presets) so it
# runs *before* compiler detection. Stock macOS ships Apple Clang as `clang`,
# which lacks a matching `clang-tidy`, a static UBSan runtime, and the same
# libc++ as the rest of the toolchain. The `clang-debug` / `clang-release`
# presets are meant to use Homebrew's LLVM clang on every platform, so on Apple
# we point the compiler, archiver, and linker tools at the Homebrew install.
#
# The Homebrew prefix is *discovered*, never hardcoded: it differs between
# Apple-silicon (/opt/homebrew) and Intel (/usr/local) Macs, and the user may
# have a custom HOMEBREW_PREFIX. Resolution order:
#   1. An explicit -DHOMEBREW_LLVM_PREFIX=... (highest priority).
#   2. `brew --prefix llvm` (authoritative; handles versioned kegs too).
#   3. Well-known fallbacks under $HOMEBREW_PREFIX / the per-arch defaults.
#
# This file is a no-op on non-Apple platforms, so the same presets keep using
# the plain `clang` / `clang++` on the PATH under Linux.

if(NOT APPLE)
    return()
endif()

# Guard against re-entry: CMake includes the toolchain file multiple times
# (once per try_compile), and we only need to resolve the prefix once.
if(DEFINED CORO_HOMEBREW_LLVM_PREFIX)
    set(_brew_llvm "${CORO_HOMEBREW_LLVM_PREFIX}")
else()
    set(_brew_llvm "")

    # (1) Explicit override.
    if(DEFINED HOMEBREW_LLVM_PREFIX AND EXISTS "${HOMEBREW_LLVM_PREFIX}/bin/clang")
        set(_brew_llvm "${HOMEBREW_LLVM_PREFIX}")
    endif()

    # (2) Ask Homebrew directly. `brew --prefix llvm` resolves the active keg,
    #     including versioned formulae (llvm@NN), without any path guessing.
    if(NOT _brew_llvm)
        find_program(CORO_BREW_EXE NAMES brew
            PATHS /opt/homebrew/bin /usr/local/bin ENV PATH)
        if(CORO_BREW_EXE)
            execute_process(
                COMMAND "${CORO_BREW_EXE}" --prefix llvm
                OUTPUT_VARIABLE _brew_llvm_out
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _brew_rc)
            if(_brew_rc EQUAL 0 AND EXISTS "${_brew_llvm_out}/bin/clang")
                set(_brew_llvm "${_brew_llvm_out}")
            endif()
        endif()
    endif()

    # (3) Well-known fallbacks: the generic Homebrew prefix, then the per-arch
    #     defaults (Apple silicon vs. Intel). Data-driven: extend the list, not
    #     the logic, to support another layout.
    if(NOT _brew_llvm)
        set(_brew_prefix_candidates "")
        if(DEFINED ENV{HOMEBREW_PREFIX})
            list(APPEND _brew_prefix_candidates "$ENV{HOMEBREW_PREFIX}")
        endif()
        list(APPEND _brew_prefix_candidates "/opt/homebrew" "/usr/local")
        foreach(_prefix IN LISTS _brew_prefix_candidates)
            if(EXISTS "${_prefix}/opt/llvm/bin/clang")
                set(_brew_llvm "${_prefix}/opt/llvm")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _brew_llvm)
        message(FATAL_ERROR
            "[HomebrewLLVM] Homebrew's LLVM clang was not found. The clang-* "
            "presets use Homebrew LLVM on macOS (not Apple Clang). Install it "
            "with `brew install llvm`, or pass -DHOMEBREW_LLVM_PREFIX=<path>.")
    endif()

    set(CORO_HOMEBREW_LLVM_PREFIX "${_brew_llvm}"
        CACHE INTERNAL "Resolved Homebrew LLVM prefix")
    message(STATUS "[HomebrewLLVM] Using Homebrew LLVM at ${_brew_llvm}")
endif()

# Point the toolchain at Homebrew LLVM. The `clang-*` presets set a *bare*
# `clang` / `clang++`, which on stock macOS resolves to Apple Clang — exactly
# what we must avoid. So we steer those bare names to the Homebrew binaries,
# while still respecting an explicit absolute path the user passed by hand
# (e.g. -DCMAKE_CXX_COMPILER=/path/to/clang++), detected by the leading slash.
foreach(_lang IN ITEMS "C:clang" "CXX:clang++")
    string(REPLACE ":" ";" _pair "${_lang}")
    list(GET _pair 0 _var)
    list(GET _pair 1 _bin)
    set(_current "${CMAKE_${_var}_COMPILER}")
    if(NOT _current OR NOT _current MATCHES "/")
        set(CMAKE_${_var}_COMPILER "${_brew_llvm}/bin/${_bin}"
            CACHE FILEPATH "${_bin}" FORCE)
    endif()
endforeach()

# Prefer the matching LLVM binutils when present (archiver, ranlib) so static
# archives are produced by the same toolchain that compiled the objects.
foreach(_tool IN ITEMS AR:llvm-ar RANLIB:llvm-ranlib)
    string(REPLACE ":" ";" _pair "${_tool}")
    list(GET _pair 0 _var)
    list(GET _pair 1 _bin)
    if(NOT CMAKE_${_var} AND EXISTS "${_brew_llvm}/bin/${_bin}")
        set(CMAKE_${_var} "${_brew_llvm}/bin/${_bin}" CACHE FILEPATH "${_bin}" FORCE)
    endif()
endforeach()

# Steer clang-tidy to Homebrew's so it matches the compiler version. A bare
# `find_program(clang-tidy)` (in ClangTidy.cmake) may otherwise miss it
# entirely (Apple Clang ships no clang-tidy) or pick a mismatched one. We only
# pre-seed the cache entry ClangTidy.cmake reads; it still honours an explicit
# user override.
if(NOT CLANG_TIDY_EXE AND EXISTS "${_brew_llvm}/bin/clang-tidy")
    set(CLANG_TIDY_EXE "${_brew_llvm}/bin/clang-tidy"
        CACHE FILEPATH "Path to clang-tidy executable")
endif()

# Note: Homebrew's clang driver already links against, and bakes an rpath to,
# its own lib/c++ (and the sanitizer runtimes), so no extra -L / -rpath flags
# are needed here — adding them produces "duplicate -rpath" linker warnings.
