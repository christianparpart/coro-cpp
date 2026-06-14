// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>
#include <Coro/WhenAny.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{

Coro::Task<std::string> AfterSleep(Coro::IScheduler& s, std::chrono::milliseconds d, std::string v)
{
    co_await Coro::Sleep(s, d);
    co_return v;
}

/// Like @ref AfterSleep, but additionally holds @p guard in its frame and
/// reports completion through @p finished — used to observe that a losing
/// child keeps running after the winner resolved and that its detached
/// frame is reclaimed once it completes.
/// @param s Scheduler driving the sleep.
/// @param d Sleep duration before completing.
/// @param guard Shared token whose refcount tracks the frame's lifetime.
/// @param finished Set to true when the body runs to completion.
/// @param v Value to produce.
Coro::Task<std::string> SleepThenSet(
    Coro::IScheduler& s, std::chrono::milliseconds d, std::shared_ptr<int> guard, bool* finished, std::string v)
{
    co_await Coro::Sleep(s, d);
    *guard = 1;
    *finished = true;
    co_return v;
}

} // namespace

TEST_CASE("WhenAny resolves to the earliest completer", "[WhenAny]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto winnerIndex = std::size_t { 0 };
    auto winnerValue = std::string {};

    auto rootFn = [&]() -> Coro::Task<void> {
        auto tasks = std::vector<Coro::Task<std::string>> {};
        tasks.push_back(AfterSleep(scheduler, 100ms, "slow"));
        tasks.push_back(AfterSleep(scheduler, 25ms, "fast"));
        tasks.push_back(AfterSleep(scheduler, 60ms, "mid"));
        auto result = co_await Coro::WhenAny(scheduler, std::move(tasks));
        winnerIndex = result.index;
        winnerValue = std::move(result.value);
    };
    auto const root = rootFn();

    scheduler.Post(root.Native());
    scheduler.AdvanceBy(25ms); // exactly the fastest child's deadline

    REQUIRE(root.IsReady());
    REQUIRE(winnerIndex == 1);
    REQUIRE(winnerValue == "fast");

    // Drain the losers so their detached wrappers run to completion and
    // reclaim themselves before the scheduler goes away.
    scheduler.AdvanceBy(75ms);
    REQUIRE(scheduler.PendingTimerCount() == 0);
}

TEST_CASE("WhenAny losers keep running and reclaim their frames", "[WhenAny]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto guard = std::make_shared<int>(0);
    auto const weak = std::weak_ptr<int> { guard };
    auto loserFinished = false;
    auto winnerValue = std::string {};

    auto rootFn = [&]() -> Coro::Task<void> {
        auto tasks = std::vector<Coro::Task<std::string>> {};
        tasks.push_back(SleepThenSet(scheduler, 100ms, std::move(guard), &loserFinished, "slow"));
        tasks.push_back(AfterSleep(scheduler, 25ms, "fast"));
        auto result = co_await Coro::WhenAny(scheduler, std::move(tasks));
        winnerValue = std::move(result.value);
    };
    auto const root = rootFn();

    scheduler.Post(root.Native());
    scheduler.AdvanceBy(25ms);

    // The parent resumed with the winner while the loser is still
    // mid-sleep: alive (frame holds the guard), timer still pending.
    REQUIRE(root.IsReady());
    REQUIRE(winnerValue == "fast");
    REQUIRE_FALSE(loserFinished);
    REQUIRE_FALSE(weak.expired());
    REQUIRE(scheduler.PendingTimerCount() == 1);

    // It must neither be destroyed while its timer is pending
    // (use-after-free) nor leak once it completes.
    scheduler.AdvanceBy(75ms);
    REQUIRE(loserFinished);  // kept running, as documented
    REQUIRE(weak.expired()); // ...and its detached frame was reclaimed
}

TEST_CASE("WhenAny rejects an empty task vector", "[WhenAny]")
{
    auto scheduler = Coro::ManualScheduler {};

    REQUIRE_THROWS_AS(Coro::WhenAny(scheduler, std::vector<Coro::Task<int>> {}), std::invalid_argument);
}
