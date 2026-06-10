# SPDX-License-Identifier: Apache-2.0
# Coro library — Task<T>, Scheduler interface, concrete EventLoop, and the
# awaiters (Sleep, WhenAll, WhenAny). Headers are listed so IDEs surface them
# in the target view; only .cpp files drive compilation.

target_sources(Coro
    PRIVATE
        EventLoop.cpp
)

target_sources(Coro
    PRIVATE
        FILE_SET coro_public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_SOURCE_DIR}/src"
        FILES
            Task.hpp
            Scheduler.hpp
            Clock.hpp
            EventLoop.hpp
            Sleep.hpp
            WhenAll.hpp
            WhenAny.hpp
            Spawn.hpp
)
