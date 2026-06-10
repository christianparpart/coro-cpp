
option(ENABLE_TIDY "Enable clang-tidy [default: OFF]" OFF)
if(ENABLE_TIDY)
    find_program(CLANG_TIDY_EXE
        NAMES clang-tidy
        DOC "Path to clang-tidy executable")
    if(NOT CLANG_TIDY_EXE)
        message(STATUS "[clang-tidy] Not found.")
    else()
        message(STATUS "[clang-tidy] found: ${CLANG_TIDY_EXE}")
        # Apply tidy only to our own source files. Without this filter, the
        # check also fires on Qt-generated QML cache loaders, MOC files, and
        # FetchContent-built dependencies (Catch2), each of which violates
        # the project rules in ways we can't fix.
        set(CMAKE_CXX_CLANG_TIDY
            "${CLANG_TIDY_EXE}"
            "--header-filter=${CMAKE_SOURCE_DIR}/src/Coro/.*")
    endif()
else()
    message(STATUS "[clang-tidy] Disabled.")
endif()

# Helper: silence clang-tidy on a target. Useful for fetched code (Catch2)
# where the diagnostics don't apply.
function(coro_disable_tidy target)
    set_target_properties("${target}" PROPERTIES CXX_CLANG_TIDY "")
endfunction()
