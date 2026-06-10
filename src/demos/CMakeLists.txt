# SPDX-License-Identifier: Apache-2.0
# Lightning-talk demo executables. Each one is intentionally tiny — the
# point is to fit on a single slide. They share the Coro library and a
# common `Fake.hpp` helper that simulates network I/O via Sleep.

add_executable(demo_typewriter         demo_typewriter.cpp)
add_executable(demo_progress           demo_progress.cpp)
add_executable(demo_sequential_fetch   demo_sequential_fetch.cpp)
add_executable(demo_when_all           demo_when_all.cpp)
add_executable(demo_spinner_cancel     demo_spinner_cancel.cpp)
add_executable(demo_dashboard          demo_dashboard.cpp)

foreach(_demo
        demo_typewriter
        demo_progress
        demo_sequential_fetch
        demo_when_all
        demo_spinner_cancel
        demo_dashboard)
    target_link_libraries(${_demo} PRIVATE Coro)
endforeach()
