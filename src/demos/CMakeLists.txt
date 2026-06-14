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

# Qt/QML demo: a pure Qt Quick app whose UI is driven by a linear C++
# coroutine sequence (FetchBackend) running on the Qt event loop via
# CoroQt's QtScheduler. Built only when Qt6 was found (CoroQt exists).
if(TARGET CoroQt)
    qt_add_executable(qt_coro_demo
        qt_coro_demo/main.cpp
        qt_coro_demo/FetchBackend.cpp
        qt_coro_demo/FetchBackend.hpp
    )
    # So Qt's generated qmltyperegistrations.cpp can #include <FetchBackend.hpp>.
    target_include_directories(qt_coro_demo PRIVATE qt_coro_demo)
    set_target_properties(qt_coro_demo PROPERTIES AUTOMOC ON)
    # Alias the QML file to the module root so its type is "Main" at
    # ':/qt/qml/CoroDemo/Main.qml' (without the subdirectory prefix), which
    # is what loadFromModule("CoroDemo", "Main") resolves.
    set_source_files_properties(qt_coro_demo/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
    qt_add_qml_module(qt_coro_demo
        URI CoroDemo
        VERSION 1.0
        QML_FILES qt_coro_demo/Main.qml
    )
    target_link_libraries(qt_coro_demo PRIVATE CoroQt Qt6::Gui Qt6::Qml Qt6::Quick)
    # Qt-generated MOC/qmlcache sources don't follow the project rules; keep
    # clang-tidy off the demo target (its own sources are tidy via the global
    # header-filter when configured with ENABLE_TIDY).
    coro_disable_tidy(qt_coro_demo)
endif()
