// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Spawn.hpp>
#include <Coro/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

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

/// Holds @p token in its coroutine frame; the token's refcount observes
/// the frame's lifetime, so a `weak_ptr` to it tells us whether the
/// detached frame was reclaimed after completion.
/// @param token Shared token stored in the frame for its whole lifetime.
/// @return A `Task<void>` whose frame owns a reference to @p token.
Coro::Task<void> HoldToken(std::shared_ptr<int> token)
{
    *token = 1;
    co_return;
}

} // namespace

TEST_CASE("IScheduler::Post(Task<void>&&) runs a detached task", "[Spawn]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto ran = false;

    // The member overload detaches the task: the frame owns itself from
    // here on and the scheduler drives it on the next drain.
    scheduler.Post(SetFlag(ran));
    REQUIRE_FALSE(ran); // lazy: not started until the scheduler runs it

    scheduler.RunUntilIdle();
    REQUIRE(ran);
}

TEST_CASE("Spawn forwards to the scheduler's Post overload", "[Spawn]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto ran = false;

    Coro::Spawn(scheduler, SetFlag(ran));
    REQUIRE_FALSE(ran);

    scheduler.RunUntilIdle();
    REQUIRE(ran);
}

TEST_CASE("Detached tasks reclaim their coroutine frame on completion", "[Spawn]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto token = std::make_shared<int>(0);
    auto const weak = std::weak_ptr<int> { token };

    scheduler.Post(HoldToken(std::move(token)));
    REQUIRE_FALSE(weak.expired()); // lazy: the frame is alive, holding the token

    scheduler.RunUntilIdle();
    REQUIRE(weak.expired()); // the body ran AND the detached frame destroyed itself
}

TEST_CASE("Post overload is reachable through the IScheduler interface", "[Spawn]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto& seam = static_cast<Coro::IScheduler&>(scheduler);
    auto ran = false;

    // Calling through the base reference must resolve to the member
    // overload, not be hidden by ManualScheduler's override of
    // Post(handle).
    seam.Post(SetFlag(ran));
    REQUIRE_FALSE(ran);

    scheduler.RunUntilIdle();
    REQUIRE(ran);
}
