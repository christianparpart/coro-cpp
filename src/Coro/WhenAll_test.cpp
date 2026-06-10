// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>
#include <Coro/WhenAll.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <tuple>

using namespace std::chrono_literals;

namespace
{

Coro::Task<int> Yield(int v)
{
    co_return v;
}

Coro::Task<std::string> AfterSleep(Coro::IScheduler& s, std::chrono::milliseconds d, std::string v)
{
    co_await Coro::Sleep(s, d);
    co_return v;
}

/// Void child that completes after a sleep and records that it ran.
/// @param s Scheduler driving the sleep.
/// @param d Sleep duration before completing.
/// @param done Set to true when the body completes.
Coro::Task<void> VoidAfterSleep(Coro::IScheduler& s, std::chrono::milliseconds d, bool* done)
{
    co_await Coro::Sleep(s, d);
    *done = true;
}

} // namespace

TEST_CASE("WhenAll collects results from ready tasks", "[WhenAll]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto got = std::tuple<int, int, int> {};

    auto const root = [&]() -> Coro::Task<void> {
        got = co_await Coro::WhenAll(scheduler, Yield(1), Yield(2), Yield(3));
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle(); // no timers involved — one drain settles everything

    REQUIRE(root.IsReady());
    REQUIRE(std::get<0>(got) == 1);
    REQUIRE(std::get<1>(got) == 2);
    REQUIRE(std::get<2>(got) == 3);
}

TEST_CASE("WhenAll runs sleepers concurrently — slowest dominates", "[WhenAll]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto got = std::tuple<std::string, std::string, std::string> {};
    auto finished = false;

    auto const root = [&]() -> Coro::Task<void> {
        got = co_await Coro::WhenAll(scheduler,
                                     AfterSleep(scheduler, 100ms, "a"),
                                     AfterSleep(scheduler, 50ms, "b"),
                                     AfterSleep(scheduler, 75ms, "c"));
        finished = true;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle(); // all three children park on their timers
    REQUIRE(scheduler.PendingTimerCount() == 3);

    // Virtual time makes the concurrency claim exact: the WhenAll
    // completes after 100ms total — the slowest child — not after the
    // 225ms sum a sequential execution would need.
    scheduler.AdvanceBy(50ms);
    REQUIRE_FALSE(finished); // "b" done, two still sleeping
    scheduler.AdvanceBy(25ms);
    REQUIRE_FALSE(finished); // "c" done, "a" still sleeping
    scheduler.AdvanceBy(25ms);
    REQUIRE(finished);

    REQUIRE(std::get<0>(got) == "a");
    REQUIRE(std::get<1>(got) == "b");
    REQUIRE(std::get<2>(got) == "c");
    REQUIRE(root.IsReady());
}

TEST_CASE("WhenAll folds void children out of the result tuple", "[WhenAll]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto voidDone = false;
    // The assignment below only compiles if the void child contributes no
    // tuple element: WhenAll(Task<int>, Task<void>, Task<string>) must
    // resolve to tuple<int, string>, as documented.
    auto got = std::tuple<int, std::string> {};

    auto const root = [&]() -> Coro::Task<void> {
        got = co_await Coro::WhenAll(
            scheduler, Yield(1), VoidAfterSleep(scheduler, 50ms, &voidDone), AfterSleep(scheduler, 25ms, "folded"));
    }();

    scheduler.Post(root.Native());
    scheduler.AdvanceBy(50ms); // crosses both sleeps in one advance

    REQUIRE(root.IsReady());
    REQUIRE(voidDone);
    REQUIRE(std::get<0>(got) == 1);
    REQUIRE(std::get<1>(got) == "folded");
}
