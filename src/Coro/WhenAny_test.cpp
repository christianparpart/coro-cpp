// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>
#include <Coro/WhenAny.hpp>

#include "ManualClock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

} // namespace

TEST_CASE("WhenAny resolves to the earliest completer", "[WhenAny]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto winnerIndex = std::size_t { 0 };
    auto winnerValue = std::string {};
    auto finished = false;

    auto root = [&]() -> Coro::Task<void> {
        auto tasks = std::vector<Coro::Task<std::string>> {};
        tasks.push_back(AfterSleep(loop, 100ms, "slow"));
        tasks.push_back(AfterSleep(loop, 25ms, "fast"));
        tasks.push_back(AfterSleep(loop, 60ms, "mid"));
        auto result = co_await Coro::WhenAny(loop, std::move(tasks));
        winnerIndex = result.index;
        winnerValue = std::move(result.value);
        finished = true;
    };

    auto task = root();
    auto handle = task.Native();
    loop.Post(handle);
    static_cast<void>(task.Release());

    auto safety = 20;
    while (!finished && safety-- > 0)
    {
        loop.RunOnce();
        if (finished)
            break;
        clock.Advance(25ms);
    }

    REQUIRE(finished);
    REQUIRE(winnerIndex == 1);
    REQUIRE(winnerValue == "fast");

    if (handle)
        handle.destroy();
}
