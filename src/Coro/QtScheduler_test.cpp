// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Qt event-loop scheduler. The whole file is guarded: the
// test binary globs every *_test.cpp, and where Qt is unavailable (the
// CORO_HAVE_QT macro is only defined when the build links CoroQt) this must
// compile to an empty translation unit — mirroring Win32MessageScheduler_test.
#if defined(CORO_HAVE_QT)

    #include <Coro/QtScheduler.hpp>
    #include <Coro/Sleep.hpp>
    #include <Coro/Task.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <chrono>
    #include <vector>

    #include <QtCore/QCoreApplication>
    #include <QtCore/QEventLoop>

using namespace std::chrono_literals;

namespace
{

/// Lazily-constructed `QCoreApplication`, shared by every case. Qt forbids
/// more than one instance per process, so it is created once and kept
/// alive for the whole test run. `argv` storage must outlive it.
/// @return The process-wide application instance.
QCoreApplication& App()
{
    static int argc = 1;
    static char arg0[] = "CoroTest";
    static char* argv[] = { static_cast<char*>(arg0), nullptr };
    static auto app = QCoreApplication { argc, static_cast<char**>(argv) };
    return app;
}

/// Pump the Qt event loop until @p done turns true or @p budget of wall
/// time elapses — a miniature stand-in for the `exec()` loop a real Qt
/// application would already be running.
/// @param done Flag the coroutine under test sets on completion.
/// @param budget Wall-clock cap so a regression cannot hang the suite.
/// @return The final value of @p done.
bool PumpUntil(bool const& done, std::chrono::milliseconds budget)
{
    auto const deadline = std::chrono::steady_clock::now() + budget;
    while (!done && std::chrono::steady_clock::now() < deadline)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done;
}

} // namespace

TEST_CASE("QtScheduler resumes a posted task via the event loop", "[QtScheduler]")
{
    App();
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::QtScheduler::Create(clock);
    REQUIRE(scheduler.has_value());

    auto ran = false;
    auto rootFn = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    auto const root = rootFn();

    (*scheduler)->Post(root.Native());
    REQUIRE_FALSE(ran); // lazy until the event loop dispatches the resume functor

    REQUIRE(PumpUntil(ran, 2000ms));
    REQUIRE(root.IsReady());
}

TEST_CASE("QtScheduler fires Sleep deadlines via QTimer", "[QtScheduler]")
{
    App();
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::QtScheduler::Create(clock);
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
    REQUIRE((*scheduler)->PendingTimerCount() == 0); // timer drained
}

TEST_CASE("QtScheduler resumes posted handles in FIFO order", "[QtScheduler]")
{
    App();
    auto clock = Coro::SystemClock {};
    auto scheduler = Coro::QtScheduler::Create(clock);
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

TEST_CASE("QtScheduler destruction drops queued continuations without resuming them", "[QtScheduler]")
{
    App();
    auto clock = Coro::SystemClock {};
    auto ran = false;
    auto rootFn = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    auto const root = rootFn();

    {
        auto scheduler = Coro::QtScheduler::Create(clock);
        REQUIRE(scheduler.has_value());
        (*scheduler)->Post(root.Native());
        // Destroyed without ever pumping: the queued resume functor must be
        // dropped, not dispatched into a dead scheduler later.
    }

    REQUIRE_FALSE(ran); // never resumed
    REQUIRE_FALSE(root.IsReady());
    // `root` going out of scope reclaims the untouched frame.
}

#endif // CORO_HAVE_QT
