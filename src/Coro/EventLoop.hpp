// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>
#include <Coro/Task.hpp>

#include <coroutine>
#include <cstddef>
#include <deque>
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
/// Each iteration: drain the ready-queue, then wait until the next
/// timer is due. When a timer fires its handle is moved from the heap
/// into the ready queue, then resumed in the next drain.
///
/// The clock is injected through the `IClock` interface — both for
/// reading time and for waiting (`IClock::WaitUntil`). Production code
/// passes `SystemClock` (which really sleeps); tests pass a
/// `ManualClock` that exposes `Advance(d)` and "waits" by jumping
/// forward, so `Run` is deterministic and instant under test.
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

    /// Drive the loop until it is fully drained: the root task has
    /// completed, every detached task posted onto the loop has run to
    /// completion, and no ready handles or timers remain. Draining is
    /// what guarantees that no coroutine handle from this run survives
    /// into a later `Run`/`RunOnce` call, and that detached frames get
    /// to reclaim themselves. Detached tasks that should not outlive
    /// the root must be made stop-aware (see the demos' use of
    /// `std::stop_source`).
    ///
    /// Waiting between timer fires goes through the injected clock seam
    /// (`IClock::WaitUntil`): `SystemClock` sleeps the thread, a
    /// `ManualClock` jumps straight to the next deadline, so tests may
    /// simply call `Run` and get deterministic, instant timer waits.
    ///
    /// @param root Root coroutine driving the demo. Must not be empty.
    ///             Taken by value; ownership is moved into the loop.
    /// @throws Re-throws an exception that escaped the root task's body
    ///         once the loop has drained, after the root frame has been
    ///         reclaimed — a failing root is never silently swallowed.
    void Run(Task<void> root);

    /// Single-step the loop: drain ready handles once and fire any
    /// timers whose deadline has already elapsed. Does NOT sleep.
    /// Exposed for tests that drive the loop with a manual clock.
    /// @return true if any handle was resumed during this step.
    bool RunOnce();

  private:
    struct TimerEntry
    {
        IClock::TimePoint when {};
        std::coroutine_handle<> handle {};
        std::size_t sequence { 0 }; ///< FIFO tiebreak for same-deadline entries.
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
