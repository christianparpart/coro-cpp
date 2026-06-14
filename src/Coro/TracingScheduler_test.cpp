// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>
#include <Coro/TracingScheduler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("TracingScheduler forwards Post to the inner scheduler and records it", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto tracing = Coro::TracingScheduler { inner };
    auto ran = false;

    auto rootFn = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    auto const root = rootFn();

    tracing.Post(root.Native());

    REQUIRE(tracing.Events().size() == 1);
    REQUIRE(tracing.Events().front().kind == Coro::TraceEvent::Kind::Post);
    REQUIRE(tracing.Events().front().handle == root.Native());
    REQUIRE(inner.ReadyCount() == 1); // really forwarded, not swallowed

    inner.RunUntilIdle();
    REQUIRE(ran);
}

TEST_CASE("TracingScheduler records ScheduleAt with the requested deadline", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto tracing = Coro::TracingScheduler { inner };
    auto reached = false;

    auto rootFn = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(tracing, 50ms);
        reached = true;
    };
    auto const root = rootFn();

    tracing.Post(root.Native());
    inner.RunUntilIdle();

    auto const events = tracing.Events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].kind == Coro::TraceEvent::Kind::Post);
    REQUIRE(events[1].kind == Coro::TraceEvent::Kind::ScheduleAt);
    REQUIRE(events[1].at == Coro::IClock::TimePoint {});
    REQUIRE(events[1].when == Coro::IClock::TimePoint {} + 50ms); // exactly one timer, exactly at t+50ms

    inner.AdvanceBy(50ms);
    REQUIRE(reached);
}

TEST_CASE("TracingScheduler reads time from the inner scheduler's clock", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto tracing = Coro::TracingScheduler { inner };

    inner.AdvanceBy(123ms);
    REQUIRE(tracing.Clock().Now() == inner.Now());
}

TEST_CASE("TracingScheduler notifies the injected sink per event, in call order", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto observed = std::vector<Coro::TraceEvent::Kind> {};
    auto tracing = Coro::TracingScheduler { inner, [&](Coro::TraceEvent const& event) { observed.push_back(event.kind); } };
    auto reached = false;

    auto rootFn = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(tracing, 10ms);
        reached = true;
    };
    auto const root = rootFn();

    tracing.Post(root.Native());
    inner.AdvanceBy(10ms);

    REQUIRE(reached);
    // The timer fire is resumed by the inner scheduler directly and is
    // deliberately not traced — only calls through this seam are.
    REQUIRE(observed == std::vector { Coro::TraceEvent::Kind::Post, Coro::TraceEvent::Kind::ScheduleAt });
}

TEST_CASE("TracingScheduler counts per kind and supports Clear", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto tracing = Coro::TracingScheduler { inner };
    auto reached = false;

    auto rootFn = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(tracing, 25ms);
        reached = true;
    };
    auto const root = rootFn();

    tracing.Post(root.Native());
    inner.AdvanceBy(25ms);

    REQUIRE(reached);
    REQUIRE(tracing.Count(Coro::TraceEvent::Kind::Post) == 1);
    REQUIRE(tracing.Count(Coro::TraceEvent::Kind::ScheduleAt) == 1);

    tracing.Clear();
    REQUIRE(tracing.Events().empty());
    REQUIRE(tracing.Count(Coro::TraceEvent::Kind::Post) == 0);
}

TEST_CASE("TracingScheduler traces detached tasks posted through the IScheduler seam", "[TracingScheduler]")
{
    auto inner = Coro::ManualScheduler {};
    auto tracing = Coro::TracingScheduler { inner };
    auto& seam = static_cast<Coro::IScheduler&>(tracing);
    auto ran = false;

    auto detached = [&]() -> Coro::Task<void> {
        ran = true;
        co_return;
    };
    seam.Post(detached()); // convenience overload must route through the traced virtual Post

    REQUIRE(tracing.Count(Coro::TraceEvent::Kind::Post) == 1);
    inner.RunUntilIdle();
    REQUIRE(ran);
}

TEST_CASE("TraceEventKindName maps kinds to their display names", "[TracingScheduler]")
{
    REQUIRE(Coro::TraceEventKindName(Coro::TraceEvent::Kind::Post) == std::string_view { "Post" });
    REQUIRE(Coro::TraceEventKindName(Coro::TraceEvent::Kind::ScheduleAt) == std::string_view { "ScheduleAt" });
}
