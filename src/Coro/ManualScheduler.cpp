// SPDX-License-Identifier: Apache-2.0
#include <Coro/ManualScheduler.hpp>

#include <algorithm>

namespace Coro
{

void ManualScheduler::Post(std::coroutine_handle<> handle)
{
    _ready.push_back(handle);
}

void ManualScheduler::ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle)
{
    _timers.Schedule(when, handle);
}

IClock const& ManualScheduler::Clock() const noexcept
{
    return _clock;
}

IClock::TimePoint ManualScheduler::Now() const noexcept
{
    return _clock.now;
}

std::size_t ManualScheduler::DrainReady()
{
    auto resumed = std::size_t { 0 };
    while (!_ready.empty())
    {
        auto const handle = _ready.front();
        _ready.pop_front();
        handle.resume();
        ++resumed;
    }
    return resumed;
}

std::size_t ManualScheduler::AdvanceTo(IClock::TimePoint target)
{
    auto resumed = DrainReady();
    // Fire timers in deadline order. Each fired timer sees the clock at
    // its own deadline, and the follow-up drain lets it schedule new
    // timers that — when they land inside the window — pop on the next
    // iteration. This is what makes chained sleeps cascade through a
    // single advance.
    while (auto const due = _timers.PopDue(target))
    {
        _clock.now = std::max(_clock.now, due.value().when);
        due.value().handle.resume();
        resumed += 1 + DrainReady();
    }
    _clock.now = std::max(_clock.now, target);
    return resumed;
}

std::size_t ManualScheduler::AdvanceBy(IClock::Duration delta)
{
    return AdvanceTo(_clock.now + delta);
}

std::size_t ManualScheduler::RunUntilIdle()
{
    return AdvanceTo(_clock.now);
}

std::size_t ManualScheduler::ReadyCount() const noexcept
{
    return _ready.size();
}

std::size_t ManualScheduler::PendingTimerCount() const noexcept
{
    return _timers.Size();
}

std::optional<IClock::TimePoint> ManualScheduler::NextDeadline() const noexcept
{
    return _timers.NextDeadline();
}

} // namespace Coro
