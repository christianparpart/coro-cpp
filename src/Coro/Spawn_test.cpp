// SPDX-License-Identifier: Apache-2.0
#include "ManualClock.hpp"

#include <Coro/EventLoop.hpp>
#include <Coro/Spawn.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{

/// Minimal side-effecting task used to observe that a detached task ran.
/// @param ran Flag set to true when the coroutine body executes.
/// @return A `Task<void>` that, once driven, sets @p ran.
Coro::Task<void> SetFlag(bool& ran)
{
    ran = true;
    co_return;
}

} // namespace

TEST_CASE("IScheduler::Post(Task<void>&&) runs a detached task", "[Spawn]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto ran = false;

    // The member overload detaches the task: ownership moves to the loop,
    // which drives it on the next step.
    loop.Post(SetFlag(ran));
    REQUIRE_FALSE(ran); // lazy: not started until the loop runs it

    loop.RunOnce();
    REQUIRE(ran);
}

TEST_CASE("Spawn forwards to the scheduler's Post overload", "[Spawn]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto ran = false;

    Coro::Spawn(loop, SetFlag(ran));
    REQUIRE_FALSE(ran);

    loop.RunOnce();
    REQUIRE(ran);
}

TEST_CASE("Post overload is reachable through the IScheduler interface", "[Spawn]")
{
    auto clock = tests::ManualClock {};
    auto loop = Coro::EventLoop { clock };
    auto& scheduler = static_cast<Coro::IScheduler&>(loop);
    auto ran = false;

    // Calling through the base reference must resolve to the member
    // overload, not be hidden by EventLoop's override of Post(handle).
    scheduler.Post(SetFlag(ran));
    REQUIRE_FALSE(ran);

    loop.RunOnce();
    REQUIRE(ran);
}
