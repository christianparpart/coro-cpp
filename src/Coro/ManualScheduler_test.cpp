// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

namespace
{

/// Records its id when run, to observe execution order.
Coro::Task<void> PushId(std::vector<int>& order, int id)
{
    order.push_back(id);
    co_return;
}

} // namespace

TEST_CASE("ManualScheduler starts at the epoch with nothing queued", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    REQUIRE(scheduler.Now() == Coro::IClock::TimePoint {});
    REQUIRE(scheduler.ReadyCount() == 0);
    REQUIRE(scheduler.PendingTimerCount() == 0);
    REQUIRE_FALSE(scheduler.NextDeadline().has_value());
    REQUIRE(scheduler.RunUntilIdle() == 0);
}

TEST_CASE("ManualScheduler resumes posted handles in FIFO order", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto order = std::vector<int> {};

    auto const a = PushId(order, 1);
    auto const b = PushId(order, 2);
    auto const c = PushId(order, 3);
    scheduler.Post(a.Native());
    scheduler.Post(b.Native());
    scheduler.Post(c.Native());

    REQUIRE(scheduler.ReadyCount() == 3);
    REQUIRE(scheduler.RunUntilIdle() == 3);
    REQUIRE(order == std::vector<int> { 1, 2, 3 });
    REQUIRE(scheduler.ReadyCount() == 0);
}

TEST_CASE("ManualScheduler exposes armed timers to assertions", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto reached = false;

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 50ms);
        reached = true;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle();

    // The Sleep parked exactly one timer at epoch+50ms — assert on the
    // *scheduling behaviour* directly instead of inferring it.
    REQUIRE_FALSE(reached);
    REQUIRE(scheduler.PendingTimerCount() == 1);
    REQUIRE(scheduler.NextDeadline() == Coro::IClock::TimePoint {} + 50ms);

    scheduler.AdvanceBy(50ms);
    REQUIRE(reached);
    REQUIRE(scheduler.PendingTimerCount() == 0);
    REQUIRE(root.IsReady());
}

TEST_CASE("ManualScheduler does not fire timers before their deadline", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto reached = false;

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 100ms);
        reached = true;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle();

    scheduler.AdvanceBy(99ms);
    REQUIRE_FALSE(reached);

    scheduler.AdvanceBy(1ms);
    REQUIRE(reached);
}

TEST_CASE("ManualScheduler cascades chained sleeps through a single advance", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto wakeTimes = std::vector<Coro::IClock::TimePoint> {};

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 30ms);
        wakeTimes.push_back(scheduler.Now());
        co_await Coro::Sleep(scheduler, 30ms);
        wakeTimes.push_back(scheduler.Now());
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle();

    // One advance crosses both deadlines; the second sleep is scheduled
    // *during* the advance and still fires, with each wake observing its
    // own deadline as the current time.
    scheduler.AdvanceBy(60ms);

    auto const epoch = Coro::IClock::TimePoint {};
    REQUIRE(wakeTimes == std::vector { epoch + 30ms, epoch + 60ms });
    REQUIRE(scheduler.Now() == epoch + 60ms);
    REQUIRE(root.IsReady());
}

TEST_CASE("ManualScheduler fires parallel timers in deadline order", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto order = std::vector<int> {};

    auto const sleeper = [&](std::chrono::milliseconds d, int id) -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, d);
        order.push_back(id);
    };

    // Detached tasks reclaim their own frames on completion.
    scheduler.Post(sleeper(50ms, 1));
    scheduler.Post(sleeper(10ms, 2));
    scheduler.Post(sleeper(30ms, 3));
    scheduler.AdvanceBy(50ms);

    REQUIRE(order == std::vector<int> { 2, 3, 1 });
}

TEST_CASE("ManualScheduler::AdvanceTo never moves time backwards", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    scheduler.AdvanceBy(100ms);
    auto const now = scheduler.Now();

    scheduler.AdvanceTo(Coro::IClock::TimePoint {} + 20ms);
    REQUIRE(scheduler.Now() == now);
}

TEST_CASE("ManualScheduler runs a detached task posted through the IScheduler seam", "[ManualScheduler]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto& seam = static_cast<Coro::IScheduler&>(scheduler);
    auto order = std::vector<int> {};

    seam.Post(PushId(order, 7));
    REQUIRE(order.empty()); // lazy until driven

    scheduler.RunUntilIdle();
    REQUIRE(order == std::vector<int> { 7 });
}
