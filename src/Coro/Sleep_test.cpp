// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("Sleep with zero duration completes immediately", "[Sleep]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto reached = false;

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 0ms);
        reached = true;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle();

    REQUIRE(reached);
    REQUIRE(root.IsReady());
    REQUIRE(scheduler.PendingTimerCount() == 0); // ready awaits never arm a timer
}

TEST_CASE("Two sequential Sleeps add their deadlines", "[Sleep]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto reached = 0;

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 30ms);
        ++reached;
        co_await Coro::Sleep(scheduler, 30ms);
        ++reached;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle(); // suspends at the first Sleep
    REQUIRE(reached == 0);

    scheduler.AdvanceBy(30ms); // first Sleep fires, suspends at the second
    REQUIRE(reached == 1);

    scheduler.AdvanceBy(30ms);
    REQUIRE(reached == 2);
    REQUIRE(root.IsReady());
}
