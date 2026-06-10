# SPDX-License-Identifier: Apache-2.0
# Catch2 test binary. Globs every *_test.cpp adjacent to source files in
# src/Coro so adding a test is a one-step operation: drop it next to the
# implementation and re-configure.

file(GLOB_RECURSE CORO_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/Coro/*_test.cpp"
)

add_executable(CoroTest
    test_main.cpp
    ${CORO_TEST_SOURCES}
)

target_include_directories(CoroTest PRIVATE
    "${CMAKE_SOURCE_DIR}/src/tests"
)

target_link_libraries(CoroTest
    PRIVATE
        Coro
        Catch2::Catch2
)

# Catch2's TEST_CASE macro expands `__COUNTER__`, which recent clang
# considers a C2y extension. Disable -Wc2y-extensions on the test
# binary so -Werror doesn't trip on third-party macro hygiene — our
# own code in src/Coro is unaffected.
target_compile_options(CoroTest PRIVATE -Wno-c2y-extensions)

# Silence clang-tidy on the Catch2 generated headers via the project
# helper (the test binary itself stays under tidy via the global setup).
include(Catch)
catch_discover_tests(CoroTest)
