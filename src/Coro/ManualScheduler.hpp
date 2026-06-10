// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>
#include <Coro/TimerQueue.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <optional>

namespace Coro
{

/// Deterministic single-threaded `IScheduler` driven by *virtual time*.
///
/// Where `EventLoop` + a manual clock forces tests to interleave clock
/// advances with `RunOnce` calls, `ManualScheduler` owns its virtual
/// clock and exposes a driving API in the style of Rx's `TestScheduler`:
///
///   * @ref RunUntilIdle — drain all ready work (and timers already due)
///     without moving time;
///   * @ref AdvanceBy / @ref AdvanceTo — move virtual time forward,
///     firing every timer on the way; each timer observes
///     `Clock().Now()` equal to its own deadline, and work it schedules
///     inside the advanced window fires within the same call (so two
///     chained 30ms sleeps complete under a single `AdvanceBy(60ms)`);
///   * @ref ReadyCount / @ref PendingTimerCount / @ref NextDeadline —
///     introspection for asserting on *scheduling behaviour* directly.
///
/// Virtual time starts at the epoch (`IClock::TimePoint {}`) and only
/// moves when the test says so — no wall-clock sleeps, no flakiness.
///
/// Handles passed to `Post`/`ScheduleAt` are borrowed (or self-owning,
/// for detached tasks); like `EventLoop`, the scheduler never destroys
/// them. Work still queued when the scheduler is destroyed is dropped.
/// Determinism is the test's responsibility: a coroutine that re-posts
/// itself forever makes `RunUntilIdle` spin forever.
class ManualScheduler final: public IScheduler
{
  public:
    using IScheduler::Post; // keep the Task<void>&& convenience overload visible
    void Post(std::coroutine_handle<> handle) override;
    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) override;
    [[nodiscard]] IClock const& Clock() const noexcept override;

    /// @return The current virtual time (shorthand for `Clock().Now()`).
    [[nodiscard]] IClock::TimePoint Now() const noexcept;

    /// Resume ready handles — including timers already due at the current
    /// virtual time and any work they post — until nothing is runnable.
    /// Virtual time does not move.
    /// @return Number of coroutine resumptions performed.
    std::size_t RunUntilIdle();

    /// Advance virtual time by @p delta, firing every timer whose
    /// deadline falls inside the window (see @ref AdvanceTo).
    /// @param delta Amount of virtual time to advance by.
    /// @return Number of coroutine resumptions performed.
    std::size_t AdvanceBy(IClock::Duration delta);

    /// Advance virtual time to @p target. Pending work is drained first;
    /// then each timer with deadline <= @p target fires in deadline order
    /// with the clock set to that deadline, and work it schedules inside
    /// the window fires in turn. Finally the clock lands on @p target.
    /// Targets in the past drain ready work but never move time backwards.
    /// @param target Virtual time to advance to.
    /// @return Number of coroutine resumptions performed.
    std::size_t AdvanceTo(IClock::TimePoint target);

    /// @return Number of handles waiting in the ready queue.
    [[nodiscard]] std::size_t ReadyCount() const noexcept;

    /// @return Number of pending (not yet due) timer entries.
    [[nodiscard]] std::size_t PendingTimerCount() const noexcept;

    /// @return The earliest pending timer deadline, or `std::nullopt`
    ///         when no timers are pending.
    [[nodiscard]] std::optional<IClock::TimePoint> NextDeadline() const noexcept;

  private:
    /// `IClock` reading the scheduler's virtual time. Waiting jumps the
    /// clock straight to the deadline, like the test `ManualClock`.
    class VirtualClock final: public IClock
    {
      public:
        [[nodiscard]] TimePoint Now() const noexcept override
        {
            return now;
        }

        /// "Waits" by jumping to @p deadline (never backwards).
        /// @param deadline Time point to advance the clock to.
        void WaitUntil(TimePoint deadline) noexcept override
        {
            now = std::max(now, deadline);
        }

        TimePoint now {}; ///< Current virtual time; starts at the epoch.
    };

    /// Resume everything in the ready queue (and whatever it posts)
    /// until the queue is empty. Does not touch timers.
    /// @return Number of coroutine resumptions performed.
    std::size_t DrainReady();

    VirtualClock _clock;
    std::deque<std::coroutine_handle<>> _ready;
    TimerQueue _timers;
};

} // namespace Coro
