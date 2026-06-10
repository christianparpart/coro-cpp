// SPDX-License-Identifier: Apache-2.0
#include <Coro/Task.hpp>
#include <Coro/TimerQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

namespace
{

/// Never-resumed coroutine whose handle serves as a distinguishable
/// opaque value for queue entries. The owning Task reclaims the frame.
Coro::Task<void> Noop()
{
    co_return;
}

constexpr auto Epoch = Coro::IClock::TimePoint {};

/// Empty-entry fallback for `std::optional::value_or`: its null handle and
/// epoch deadline can never compare equal to a real scheduled entry, so
/// assertions on the fallback fail loudly instead of dereferencing an
/// empty optional (keeps `bugprone-unchecked-optional-access` provably
/// satisfied without unchecked `->`/`value()` access).
constexpr auto NoEntry = Coro::TimerQueue::Entry {};

} // namespace

TEST_CASE("TimerQueue pops entries in deadline order regardless of insertion order", "[TimerQueue]")
{
    auto queue = Coro::TimerQueue {};
    auto const late = Noop();
    auto const early = Noop();

    queue.Schedule(Epoch + 50ms, late.Native());
    queue.Schedule(Epoch + 10ms, early.Native());

    auto const first = queue.PopDue(Epoch + 100ms).value_or(NoEntry);
    REQUIRE(first.handle == early.Native());
    REQUIRE(first.when == Epoch + 10ms);

    auto const second = queue.PopDue(Epoch + 100ms).value_or(NoEntry);
    REQUIRE(second.handle == late.Native());
    REQUIRE(queue.Empty());
}

TEST_CASE("TimerQueue breaks deadline ties in FIFO order", "[TimerQueue]")
{
    auto queue = Coro::TimerQueue {};
    auto const a = Noop();
    auto const b = Noop();
    auto const c = Noop();

    queue.Schedule(Epoch + 10ms, a.Native());
    queue.Schedule(Epoch + 10ms, b.Native());
    queue.Schedule(Epoch + 10ms, c.Native());

    REQUIRE(queue.PopDue(Epoch + 10ms).value_or(NoEntry).handle == a.Native());
    REQUIRE(queue.PopDue(Epoch + 10ms).value_or(NoEntry).handle == b.Native());
    REQUIRE(queue.PopDue(Epoch + 10ms).value_or(NoEntry).handle == c.Native());
}

TEST_CASE("TimerQueue does not surface entries before their deadline", "[TimerQueue]")
{
    auto queue = Coro::TimerQueue {};
    auto const task = Noop();

    queue.Schedule(Epoch + 30ms, task.Native());

    REQUIRE_FALSE(queue.PopDue(Epoch).has_value());
    REQUIRE_FALSE(queue.PopDue(Epoch + 29ms).has_value());
    REQUIRE(queue.Size() == 1);
    REQUIRE(queue.PopDue(Epoch + 30ms).has_value()); // inclusive deadline
}

TEST_CASE("TimerQueue reports the earliest pending deadline", "[TimerQueue]")
{
    auto queue = Coro::TimerQueue {};
    REQUIRE_FALSE(queue.NextDeadline().has_value());
    REQUIRE(queue.Empty());
    REQUIRE(queue.Size() == 0);

    auto const a = Noop();
    auto const b = Noop();
    queue.Schedule(Epoch + 40ms, a.Native());
    REQUIRE(queue.NextDeadline() == Epoch + 40ms);

    queue.Schedule(Epoch + 20ms, b.Native());
    REQUIRE(queue.NextDeadline() == Epoch + 20ms);
    REQUIRE(queue.Size() == 2);

    static_cast<void>(queue.PopDue(Epoch + 20ms));
    REQUIRE(queue.NextDeadline() == Epoch + 40ms);
}
