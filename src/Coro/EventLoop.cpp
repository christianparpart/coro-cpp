// SPDX-License-Identifier: Apache-2.0
#include <Coro/EventLoop.hpp>

#include <cassert>
#include <cstddef>
#include <exception>
#include <ranges>
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
    _timers.Schedule(when, handle);
}

void EventLoop::DrainExpiredTimers()
{
    auto const now = _clock.Now();
    while (auto const due = _timers.PopDue(now))
        _ready.push_back(due.value().handle);
}

bool EventLoop::RunOnce()
{
    DrainExpiredTimers();
    // Snapshot the ready queue size so handles posted by resumed
    // coroutines run on the next iteration, not this one — this keeps
    // a single `RunOnce` call bounded.
    auto const drainCount = _ready.size();
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, drainCount))
    {
        auto const handle = _ready.front();
        _ready.pop_front();
        handle.resume();
    }
    return drainCount > 0;
}

void EventLoop::Run(Task<void> root)
{
    auto const handle = std::move(root).Release();
    assert(handle && "Coro::EventLoop::Run requires a non-empty root task");
    Post(handle);

    // Pump until the loop is fully drained: nothing ready, no pending
    // timers. Draining — rather than stopping the moment the root is done
    // — is what lets detached tasks run to completion and reclaim their
    // frames, and it guarantees no handle from this run is left behind to
    // dangle into a later Run/RunOnce call.
    while (true)
    {
        DrainExpiredTimers();

        if (!_ready.empty())
        {
            auto const next = _ready.front();
            _ready.pop_front();
            next.resume();
            continue;
        }

        auto const next = _timers.NextDeadline();
        if (!next)
            break; // nothing ready, no future timers — loop has drained.

        // Wait for the next timer through the injected clock seam:
        // SystemClock sleeps the thread, a manual test clock jumps
        // straight to the deadline (keeping tests deterministic).
        _clock.WaitUntil(next.value());
    }

    // Reclaim the root frame first so it (and everything it owns) is
    // released even when the root failed; then surface the failure.
    auto exception = std::move(handle.promise().exception);
    handle.destroy();
    if (exception)
        std::rethrow_exception(exception);
}

} // namespace Coro
