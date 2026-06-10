// SPDX-License-Identifier: Apache-2.0
#include "ManualClock.hpp"

#include <Coro/EventLoop.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

namespace
{

Coro::Task<int> MakeFortyTwo()
{
    co_return 42;
}

Coro::Task<std::string> Concat(std::string a, std::string b)
{
    co_return a + b;
}

Coro::Task<void> ReturnVoid(int* sideEffect)
{
    *sideEffect = 7;
    co_return;
}

Coro::Task<int> Throwing()
{
    throw std::runtime_error { "boom" };
    co_return 0; // unreachable
}

/// Drives a task to completion by polling the inner handle. The Task
/// types are lazy, so we resume the handle manually for tests that do
/// not need a scheduler.
template <typename T>
T SyncDrive(Coro::Task<T> task)
{
    auto handle = task.Native();
    handle.resume();
    REQUIRE(handle.done());
    if constexpr (std::is_same_v<T, void>)
    {
        if (auto const& e = handle.promise().exception; e)
            std::rethrow_exception(e);
    }
    else
    {
        if (auto const& e = handle.promise().exception; e)
            std::rethrow_exception(e);
        return std::move(std::get<1>(handle.promise().result));
    }
}

} // namespace

TEST_CASE("Task<int> returns its co_return value", "[Task]")
{
    REQUIRE(SyncDrive(MakeFortyTwo()) == 42);
}

TEST_CASE("Task<string> carries non-trivial values", "[Task]")
{
    REQUIRE(SyncDrive(Concat("hello, ", "world")) == "hello, world");
}

TEST_CASE("Task<void> runs side effects", "[Task]")
{
    auto sideEffect = 0;
    SyncDrive(ReturnVoid(&sideEffect));
    REQUIRE(sideEffect == 7);
}

TEST_CASE("Task propagates exceptions", "[Task]")
{
    REQUIRE_THROWS_AS(SyncDrive(Throwing()), std::runtime_error);
}

TEST_CASE("Default-constructed Task is empty and safe to destroy", "[Task]")
{
    auto task = Coro::Task<int> {};
    REQUIRE(task.IsReady());
}

TEST_CASE("Awaiting an empty Task<void> completes immediately", "[Task]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto reached = false;

    auto root = [&]() -> Coro::Task<void> {
        co_await Coro::Task<void> {};
        reached = true;
    };

    loop.Run(root());
    REQUIRE(reached);
}

TEST_CASE("Awaiting an empty Task<T> throws std::logic_error", "[Task]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto caughtLogicError = false;

    auto root = [&]() -> Coro::Task<void> {
        try
        {
            [[maybe_unused]] auto const value = co_await Coro::Task<int> {};
        }
        catch (std::logic_error const&)
        {
            caughtLogicError = true;
        }
    };

    loop.Run(root());
    REQUIRE(caughtLogicError);
}
