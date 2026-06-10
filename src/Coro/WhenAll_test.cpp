// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>
#include <Coro/WhenAll.hpp>

#include "ManualClock.hpp"

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

} // namespace

TEST_CASE("WhenAll collects results from ready tasks", "[WhenAll]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto got = std::tuple<int, int, int> {};

    auto root = [&]() -> Coro::Task<void> {
        got = co_await Coro::WhenAll(loop, Yield(1), Yield(2), Yield(3));
    };

    loop.Run(root());

    REQUIRE(std::get<0>(got) == 1);
    REQUIRE(std::get<1>(got) == 2);
    REQUIRE(std::get<2>(got) == 3);
}

TEST_CASE("WhenAll runs sleepers concurrently — slowest dominates", "[WhenAll]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto got = std::tuple<std::string, std::string, std::string> {};
    auto finished = false;

    auto root = [&]() -> Coro::Task<void> {
        got = co_await Coro::WhenAll(loop,
                                     AfterSleep(loop, 100ms, "a"),
                                     AfterSleep(loop, 50ms, "b"),
                                     AfterSleep(loop, 75ms, "c"));
        finished = true;
    };

    auto task = root();
    auto handle = task.Native();
    loop.Post(handle);
    static_cast<void>(task.Release());

    // Drive: each RunOnce drains one batch of ready handles.
    auto safety = 20;
    while (!finished && safety-- > 0)
    {
        loop.RunOnce();
        if (finished)
            break;
        clock.Advance(25ms);
    }

    REQUIRE(finished);
    REQUIRE(std::get<0>(got) == "a");
    REQUIRE(std::get<1>(got) == "b");
    REQUIRE(std::get<2>(got) == "c");

    if (handle)
        handle.destroy();
}
