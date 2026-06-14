// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the shared external-pump timer algorithm. These pin the
// behaviour both Win32MessageScheduler and QtScheduler delegate to, on
// every platform (the schedulers' own tests only run where their framework
// exists). The transport seams are faked with recorders, so the algorithm
// is exercised without any real OS timer or message pump.
#include <Coro/PumpTimerDriver.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <vector>

#include <ManualClock.hpp>

using namespace std::chrono_literals;

namespace
{

/// A trivial coroutine whose handle records its id on resume, so tests can
/// observe which continuation the driver posted and in what order.
Coro::Task<void> PushId(std::vector<int>& order, int id)
{
    order.push_back(id);
    co_return;
}

/// Recording fakes for the driver's three transport seams. `posted` maps a
/// resumed handle back to its id via the `order` vector when the test runs
/// the handle; `armDelays` and `cancels` count timer manipulation so tests
/// can assert on the (otherwise invisible) re-arm decisions.
struct Transport
{
    std::vector<std::coroutine_handle<>> posted;      ///< Handles handed to the post seam.
    std::vector<std::chrono::milliseconds> armDelays; ///< Delays the timer was armed with.
    std::size_t cancels { 0 };                        ///< Number of timer cancellations.

    /// @return A driver wired to record into this transport.
    [[nodiscard]] Coro::Detail::PumpTimerDriver Driver(Coro::IClock& clock)
    {
        return Coro::Detail::PumpTimerDriver {
            clock,
            [this](std::coroutine_handle<> handle) { posted.push_back(handle); },
            [this](std::chrono::milliseconds delay) { armDelays.push_back(delay); },
            [this] { cancels += 1; },
        };
    }
};

} // namespace

TEST_CASE("PumpTimerDriver posts an already-due deadline immediately without arming", "[PumpTimerDriver]")
{
    auto clock = tests::ManualClock {};
    auto transport = Transport {};
    auto driver = transport.Driver(clock);

    auto order = std::vector<int> {};
    auto const task = PushId(order, 1);

    // Deadline in the past: must be posted at once, skipping the timer.
    driver.ScheduleAt(clock.Now() - 1ms, task.Native());

    REQUIRE(transport.posted.size() == 1);
    REQUIRE(transport.armDelays.empty());
    REQUIRE(driver.PendingCount() == 0);
}

TEST_CASE("PumpTimerDriver arms the timer with the ceil-rounded delay for a future deadline", "[PumpTimerDriver]")
{
    auto clock = tests::ManualClock {};
    auto transport = Transport {};
    auto driver = transport.Driver(clock);

    auto order = std::vector<int> {};
    auto const task = PushId(order, 1);

    // 1500us rounds up to 2ms; nothing is posted yet, one timer is armed.
    driver.ScheduleAt(clock.Now() + 1500us, task.Native());

    REQUIRE(transport.posted.empty());
    REQUIRE(transport.armDelays == std::vector<std::chrono::milliseconds> { 2ms });
    REQUIRE(driver.PendingCount() == 1);
}

TEST_CASE("PumpTimerDriver re-arms to the earliest of several pending deadlines", "[PumpTimerDriver]")
{
    auto clock = tests::ManualClock {};
    auto transport = Transport {};
    auto driver = transport.Driver(clock);

    auto order = std::vector<int> {};
    auto const a = PushId(order, 1);
    auto const b = PushId(order, 2);

    driver.ScheduleAt(clock.Now() + 50ms, a.Native());
    driver.ScheduleAt(clock.Now() + 10ms, b.Native()); // earlier — should re-arm

    REQUIRE(driver.PendingCount() == 2);
    REQUIRE(transport.armDelays == std::vector<std::chrono::milliseconds> { 50ms, 10ms });
}

TEST_CASE("PumpTimerDriver drains due entries in FIFO order and re-arms for the rest", "[PumpTimerDriver]")
{
    auto clock = tests::ManualClock {};
    auto transport = Transport {};
    auto driver = transport.Driver(clock);

    auto order = std::vector<int> {};
    auto const a = PushId(order, 1);
    auto const b = PushId(order, 2);
    auto const c = PushId(order, 3);

    driver.ScheduleAt(clock.Now() + 10ms, a.Native());
    driver.ScheduleAt(clock.Now() + 10ms, b.Native()); // same deadline — FIFO tiebreak
    driver.ScheduleAt(clock.Now() + 30ms, c.Native());

    // Advance past the first two deadlines but not the third, then fire.
    clock.Advance(10ms);
    driver.OnTimerExpired();

    // a and b posted in insertion order; c still pending and re-armed.
    REQUIRE(transport.posted.size() == 2);
    REQUIRE(transport.posted[0] == a.Native());
    REQUIRE(transport.posted[1] == b.Native());
    REQUIRE(driver.PendingCount() == 1);
    // Last arm reflects c's remaining 20ms (30ms deadline, clock at 10ms).
    REQUIRE(transport.armDelays.back() == 20ms);
}

TEST_CASE("PumpTimerDriver cancels the timer once the last entry is drained", "[PumpTimerDriver]")
{
    auto clock = tests::ManualClock {};
    auto transport = Transport {};
    auto driver = transport.Driver(clock);

    auto order = std::vector<int> {};
    auto const task = PushId(order, 1);

    driver.ScheduleAt(clock.Now() + 10ms, task.Native());
    REQUIRE(transport.cancels == 0);

    clock.Advance(10ms);
    driver.OnTimerExpired();

    REQUIRE(transport.posted.size() == 1);
    REQUIRE(driver.PendingCount() == 0);
    REQUIRE(transport.cancels == 1); // empty queue → timer cancelled
}
