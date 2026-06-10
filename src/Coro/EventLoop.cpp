// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>

#include <algorithm>
#include <cassert>
#include <thread>
#include <utility>

namespace Coro
{

EventLoop::EventLoop(IClock& clock) noexcept:
    _clock { clock }
{
}

void EventLoop::Post(std::coroutine_handle<> handle)
{
    _ready.push_back(handle);
}

void EventLoop::ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle)
{
    _timers.push_back(TimerEntry { .when = when, .handle = handle, .sequence = _nextTimerSequence++ });
    std::ranges::push_heap(_timers, TimerLater {});
}

void EventLoop::DrainExpiredTimers()
{
    auto const now = _clock.Now();
    while (!_timers.empty() && _timers.front().when <= now)
    {
        std::ranges::pop_heap(_timers, TimerLater {});
        auto entry = _timers.back();
        _timers.pop_back();
        _ready.push_back(entry.handle);
    }
}

bool EventLoop::RunOnce()
{
    DrainExpiredTimers();
    bool resumedAny = false;
    // Snapshot the ready queue size so handles posted by resumed
    // coroutines run on the next iteration, not this one — this keeps
    // a single `RunOnce` call bounded.
    auto const drainCount = _ready.size();
    for (std::size_t i = 0; i < drainCount; ++i)
    {
        auto handle = _ready.front();
        _ready.pop_front();
        resumedAny = true;
        handle.resume();
    }
    return resumedAny;
}

void EventLoop::Run(Task<void> root)
{
    auto handle = root.Release();
    assert(handle && "Coro::EventLoop::Run requires a non-empty root task");
    Post(handle);

    while (true)
    {
        DrainExpiredTimers();

        if (_ready.empty())
        {
            if (_timers.empty())
                break; // nothing ready, no future timers — loop drains.
            // Block until the next timer is due. With SystemClock this
            // is wall-clock sleep; with a ManualClock the test is
            // expected to never reach this branch (it advances the
            // clock so a timer is already due before re-entering).
            std::this_thread::sleep_until(_timers.front().when);
            continue;
        }

        auto next = _ready.front();
        _ready.pop_front();
        next.resume();

        if (handle.done())
            break;
    }

    if (handle)
        handle.destroy();
}

} // namespace Coro
