// SPDX-License-Identifier: Apache-2.0
#include "ManualClock.hpp"

#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <vector>

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

TEST_CASE("EventLoop::Run rethrows an exception escaping the root task", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };

    auto root = []() -> Coro::Task<void> {
        throw std::runtime_error { "boom" };
        co_return; // unreachable; marks the function as a coroutine
    };

    REQUIRE_THROWS_AS(loop.Run(root()), std::runtime_error);
}

TEST_CASE("EventLoop::Run completes timer waits deterministically under a manual clock", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto reached = false;

    auto root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(loop, 100ms);
        reached = true;
    };

    // Run waits through the injected clock seam, so the manual clock
    // jumps straight to the deadline instead of busy-spinning forever.
    loop.Run(root());

    REQUIRE(reached);
    REQUIRE(clock.Now() == Coro::IClock::TimePoint {} + 100ms);
}

TEST_CASE("EventLoop::Run drains detached work and leaves the loop reusable", "[EventLoop]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto detachedFinished = false;
    auto secondRootRan = false;

    auto sleeper = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(loop, 200ms);
        detachedFinished = true;
    };
    auto root = [&]() -> Coro::Task<void> {
        loop.Post(sleeper());
        co_await Coro::Sleep(loop, 10ms);
    };

    // The detached sleeper outlives the root; Run keeps pumping until it
    // completed, so no handle from this run survives into the next one.
    loop.Run(root());
    REQUIRE(detachedFinished);

    auto secondRoot = [&]() -> Coro::Task<void> {
        secondRootRan = true;
        co_return;
    };

    loop.Run(secondRoot());
    REQUIRE(secondRootRan);
}
