// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include "ManualClock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("EventLoop resumes a root task with no awaits", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto ran = false;

    auto root = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };

    loop.Run(root());
    REQUIRE(ran);
}

TEST_CASE("EventLoop drives a sleep using the injected clock", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto reached = false;

    auto root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(loop, 100ms);
        reached = true;
    };

    auto task = root();
    auto handle = task.Native();
    loop.Post(handle);
    static_cast<void>(task.Release());

    // Before time advances, the loop has no ready work and the timer
    // is still in the future, so RunOnce drains nothing.
    loop.RunOnce(); // runs the root up to the Sleep suspend
    REQUIRE_FALSE(reached);

    // Advance partway — still pending.
    clock.Advance(50ms);
    loop.RunOnce();
    REQUIRE_FALSE(reached);

    // Cross the deadline — the timer should fire on the next step.
    clock.Advance(60ms);
    loop.RunOnce();
    REQUIRE(reached);

    if (handle)
        handle.destroy();
}

TEST_CASE("EventLoop preserves FIFO order for ready handles", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto order = std::vector<int> {};

    auto child = [&](int id) -> Coro::Task<void> {
        order.push_back(id);
        co_return;
    };

    // Post three handles directly via the scheduler interface.
    auto a = child(1);
    auto b = child(2);
    auto c = child(3);
    loop.Post(a.Native());
    loop.Post(b.Native());
    loop.Post(c.Native());

    loop.RunOnce();

    REQUIRE(order == std::vector<int> { 1, 2, 3 });
}
