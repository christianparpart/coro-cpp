# SPDX-License-Identifier: Apache-2.0
# Coro library — Task<T>, Scheduler interface, concrete EventLoop, and the
# awaiters (Sleep, WhenAll, WhenAny). Headers are listed so IDEs surface them
# in the target view; only .cpp files drive compilation.

target_sources(Coro
    PRIVATE
        EventLoop.cpp
        ManualScheduler.cpp
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
            ManualScheduler.hpp
            PumpTimerDriver.hpp
            Sleep.hpp
            TimerQueue.hpp
            TracingScheduler.hpp
            WhenAll.hpp
            WhenAny.hpp
            Spawn.hpp
)

# Win32 message-pump scheduler: lets MFC / raw Win32 / WTL applications
# drive Coro tasks from their existing GUI message loop. Only meaningful
# (and only compilable) where <windows.h> exists.
if(WIN32)
    target_sources(Coro
        PRIVATE
            Win32MessageScheduler.cpp
    )
    target_sources(Coro
        PRIVATE
            FILE_SET coro_public_headers
            TYPE HEADERS
            BASE_DIRS "${CMAKE_SOURCE_DIR}/src"
            FILES
                Win32MessageScheduler.hpp
    )
endif()

# Qt event-loop scheduler: lets Qt Widgets / Qt Quick (QML) applications
# drive Coro tasks from their existing event loop. Compiled into the
# optional CoroQt library (created in the top-level CMakeLists when Qt6 is
# found), so the core Coro library stays Qt-free. The sources sit here next
# to the other schedulers; the test glob picks up QtScheduler_test.cpp.
if(TARGET CoroQt)
    target_sources(CoroQt
        PRIVATE
            QtScheduler.cpp
    )
    target_sources(CoroQt
        PRIVATE
            FILE_SET coroqt_public_headers
            TYPE HEADERS
            BASE_DIRS "${CMAKE_SOURCE_DIR}/src"
            FILES
                QtScheduler.hpp
    )
endif()
