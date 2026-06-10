// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include "ManualClock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("Sleep with zero duration completes immediately", "[Sleep]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto reached = false;

    auto root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(loop, 0ms);
        reached = true;
    };

    loop.Run(root());
    REQUIRE(reached);
}

TEST_CASE("Two sequential Sleeps add their deadlines", "[Sleep]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto reached = 0;

    auto root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(loop, 30ms);
        ++reached;
        co_await Coro::Sleep(loop, 30ms);
        ++reached;
    };

    auto task = root();
    auto handle = task.Native();
    loop.Post(handle);
    static_cast<void>(task.Release());

    loop.RunOnce(); // suspends at first Sleep
    REQUIRE(reached == 0);

    clock.Advance(30ms);
    loop.RunOnce(); // first Sleep fires, suspends at second Sleep
    REQUIRE(reached == 1);

    clock.Advance(30ms);
    loop.RunOnce();
    REQUIRE(reached == 2);

    if (handle)
        handle.destroy();
}
