// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Win32 message-pump scheduler. The whole file is guarded:
// the test binary globs every *_test.cpp, and on non-Windows hosts this
// must compile to an empty translation unit (the scheduler itself is
// only built under if(WIN32)).
#ifdef _WIN32

    #include <Coro/Sleep.hpp>
    #include <Coro/Task.hpp>
    #include <Coro/Win32MessageScheduler.hpp>

    #include <catch2/catch_test_macros.hpp>

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <chrono>
    #include <vector>

    #include <windows.h>

using namespace std::chrono_literals;

namespace
{

/// Pump the calling thread's message queue until @p done turns true or
/// @p budget of wall time elapses — a miniature stand-in for the message
/// loop an MFC application would already be running.
/// @param done Flag the coroutine under test sets on completion.
/// @param budget Wall-clock cap so a regression cannot hang the suite.
/// @return The final value of @p done.
bool PumpUntil(bool const& done, std::chrono::milliseconds budget)
{
    auto const deadline = std::chrono::steady_clock::now() + budget;
    auto message = MSG {};
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        while (!done && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return done;
}

} // namespace

TEST_CASE("Win32MessageScheduler resumes a posted task via the pump", "[Win32MessageScheduler]")
{
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::Win32MessageScheduler::Create(clock);
    REQUIRE(scheduler.has_value());

    auto ran = false;
    auto rootFn = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    auto const root = rootFn();

    (*scheduler)->Post(root.Native());
    REQUIRE_FALSE(ran); // lazy until the pump dispatches the resume message

    REQUIRE(PumpUntil(ran, 2000ms));
    REQUIRE(root.IsReady());
}

TEST_CASE("Win32MessageScheduler fires Sleep deadlines via WM_TIMER", "[Win32MessageScheduler]")
{
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::Win32MessageScheduler::Create(clock);
    REQUIRE(scheduler.has_value());

    auto finished = false;
    auto rootFn = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(**scheduler, 50ms);
        finished = true;
    };
    auto const root = rootFn();

    auto const start = clock.Now();
    (*scheduler)->Post(root.Native());

    REQUIRE(PumpUntil(finished, 5000ms));
    REQUIRE(clock.Now() - start >= 50ms);            // never early
    REQUIRE((*scheduler)->PendingTimerCount() == 0); // timer slot drained
}

TEST_CASE("Win32MessageScheduler resumes posted handles in FIFO order", "[Win32MessageScheduler]")
{
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::Win32MessageScheduler::Create(clock);
    REQUIRE(scheduler.has_value());

    auto order = std::vector<int> {};
    auto allDone = false;
    auto const child = [&](int id) -> Coro::Task<void> {
        order.push_back(id);
        if (order.size() == 3)
            allDone = true;
        co_return;
    };

    auto const a = child(1);
    auto const b = child(2);
    auto const c = child(3);
    (*scheduler)->Post(a.Native());
    (*scheduler)->Post(b.Native());
    (*scheduler)->Post(c.Native());

    REQUIRE(PumpUntil(allDone, 2000ms));
    REQUIRE(order == std::vector<int> { 1, 2, 3 });
}

TEST_CASE("Win32MessageScheduler destruction drops queued continuations without resuming them", "[Win32MessageScheduler]")
{
    auto clock = Coro::SystemClock {};
    auto ran = false;
    auto rootFn = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    auto const root = rootFn();

    {
        auto scheduler = Coro::Win32MessageScheduler::Create(clock);
        REQUIRE(scheduler.has_value());
        (*scheduler)->Post(root.Native());
        // Destroyed without ever pumping: the queued resume message must
        // be drained, not dispatched into a dead scheduler later.
    }

    REQUIRE_FALSE(ran); // never resumed
    REQUIRE_FALSE(root.IsReady());
    // `root` going out of scope reclaims the untouched frame.
}

#endif // _WIN32
