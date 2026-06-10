// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>
#include <Coro/Task.hpp>

#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

namespace Coro
{

/// Single-threaded cooperative event loop. Concrete `IScheduler` used by
/// the lightning-talk demos and the unit tests.
///
/// The loop holds two data structures:
///   * a FIFO ready-queue of coroutine handles to resume next;
///   * a min-heap of (deadline, handle) timer entries.
///
/// Each iteration: drain the ready-queue, then either sleep until the
/// next timer (in production) or fail-fast (in tests with a manual
/// clock). When a timer fires its handle is moved from the heap into
/// the ready queue, then resumed in the next drain.
///
/// The clock is injected through the `IClock` interface — production code passes `SystemClock`, 
/// tests pass a `ManualClock` that exposes `Advance(d)`.
class EventLoop final: public IScheduler
{
  public:
    /// Construct an event loop bound to the given clock.
    /// @param clock Time source used by `ScheduleAt`. Must outlive the
    ///              event loop.
    explicit EventLoop(IClock& clock) noexcept;

    EventLoop(EventLoop const&) = delete;
    EventLoop& operator=(EventLoop const&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;
    ~EventLoop() override = default;

    using IScheduler::Post; // keep the Task<void>&& convenience overload visible
    void Post(std::coroutine_handle<> handle) override;
    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) override;
    [[nodiscard]] IClock const& Clock() const noexcept override
    {
        return _clock;
    }

    /// Drive the loop to completion of the given root task.
    /// Posts the root coroutine to the ready queue and then iterates
    /// until either the loop runs dry (nothing ready, nothing
    /// scheduled) or the root task is done.
    ///
    /// The blocking strategy between timer fires is data-driven by the
    /// injected clock: with `SystemClock`, the loop sleeps until the
    /// next timer using `std::this_thread::sleep_until`. Tests that
    /// inject a `ManualClock` are expected to advance the clock
    /// themselves between `Run` calls.
    ///
    /// @param root Root coroutine driving the demo. Must not be empty.
    ///             Taken by value; ownership is moved into the loop.
    void Run(Task<void> root);

    /// Single-step the loop: drain ready handles once and fire any
    /// timers whose deadline has already elapsed. Does NOT sleep.
    /// Exposed for tests that drive the loop with a manual clock.
    /// @return true if any handle was resumed during this step.
    bool RunOnce();

  private:
    struct TimerEntry
    {
        IClock::TimePoint when;
        std::coroutine_handle<> handle;
        std::size_t sequence; ///< FIFO tiebreak for same-deadline entries.
    };

    /// Strict-weak ordering for the timer min-heap (latest first, so
    /// `std::pop_heap` yields the earliest deadline).
    struct TimerLater
    {
        [[nodiscard]] bool operator()(TimerEntry const& a, TimerEntry const& b) const noexcept
        {
            if (a.when != b.when)
                return a.when > b.when;
            return a.sequence > b.sequence;
        }
    };

    /// Pop timers whose deadline has elapsed and move their handles to
    /// the ready queue.
    void DrainExpiredTimers();

    IClock& _clock;
    std::deque<std::coroutine_handle<>> _ready;
    std::vector<TimerEntry> _timers;
    std::size_t _nextTimerSequence { 0 };
};

} // namespace Coro
