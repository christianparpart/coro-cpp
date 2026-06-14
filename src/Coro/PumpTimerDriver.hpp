// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/TimerQueue.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <utility>

namespace Coro::Detail
{

/// Drives the shared "external event-pump" timer algorithm used by every
/// scheduler that does not own a loop but instead *rides* a host one — the
/// Win32 message pump (@ref Coro::Win32MessageScheduler), the Qt event
/// loop (@ref Coro::QtScheduler), and any future GUI/framework
/// integration.
///
/// All such schedulers share the exact same logic and differ only in how
/// they resume a continuation on the pump thread and how they arm a single
/// one-shot OS timer. This driver owns the @ref TimerQueue and implements
/// that logic once; the host supplies only the three transport seams. That
/// is the AGENT.md dependency-injection rule applied to the timer
/// machinery: the algorithm depends on *interfaces* (the injected
/// `std::function`s), never on a concrete framework.
///
/// @par Algorithm
///   * @ref ScheduleAt — a deadline already due is posted immediately
///     (skipping the coarse OS timer); otherwise the handle is parked in
///     the queue and the single OS timer is re-armed to the earliest
///     deadline.
///   * @ref OnTimerExpired — the host calls this when its OS timer fires;
///     every due entry is routed *back through* the post seam (one resume
///     path, FIFO fairness with continuations posted before the tick) and
///     the timer is re-armed for the next deadline.
///
/// @par Threading
/// Single-threaded: every method must run on the host's pump thread. (The
/// post seam itself may be thread-safe — e.g. Win32 `PostMessage` — but
/// that is the host's concern, not the driver's.)
///
/// @par Borrowed handles
/// Handles are borrowed, exactly as @ref TimerQueue documents: the driver
/// never resumes or destroys them directly. On destruction, entries still
/// parked in the queue are dropped (their frames stay with their owners),
/// mirroring `EventLoop` teardown.
class PumpTimerDriver
{
  public:
    /// Resume a continuation on the host's pump thread (Win32
    /// `PostMessage`, Qt `QMetaObject::invokeMethod`, …).
    using PostFn = std::function<void(std::coroutine_handle<>)>;

    /// Arm the host's single one-shot timer to fire after @p delay. The
    /// delay is already clamped to be non-negative; the host applies any
    /// platform-specific bound (e.g. Win32 `USER_TIMER_MINIMUM`).
    using ArmTimerFn = std::function<void(std::chrono::milliseconds delay)>;

    /// Cancel the host's single one-shot timer.
    using CancelTimerFn = std::function<void()>;

    /// @param clock Time source for deadlines and due checks. Must outlive
    ///              the driver.
    /// @param post Resume-on-pump-thread seam.
    /// @param armTimer Seam arming the single OS timer for a given delay.
    /// @param cancelTimer Seam cancelling the single OS timer.
    PumpTimerDriver(IClock& clock, PostFn post, ArmTimerFn armTimer, CancelTimerFn cancelTimer) noexcept:
        _clock { clock },
        _post { std::move(post) },
        _armTimer { std::move(armTimer) },
        _cancelTimer { std::move(cancelTimer) }
    {
    }

    PumpTimerDriver(PumpTimerDriver const&) = delete;
    PumpTimerDriver& operator=(PumpTimerDriver const&) = delete;
    PumpTimerDriver(PumpTimerDriver&&) = delete;
    PumpTimerDriver& operator=(PumpTimerDriver&&) = delete;
    ~PumpTimerDriver() = default;

    /// Schedule @p handle to resume at (or shortly after) @p when. A
    /// deadline already due is posted immediately; otherwise it is parked
    /// and the single OS timer is re-armed to the earliest deadline.
    /// @param when Earliest time at which @p handle may be resumed.
    /// @param handle Suspended coroutine to resume.
    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle)
    {
        if (when <= _clock.Now())
        {
            // Already due — skip the (coarse-granularity) OS timer.
            _post(handle);
            return;
        }
        _timers.Schedule(when, handle);
        Rearm();
    }

    /// Host hook for "the OS timer fired": post every due entry back
    /// through the post seam, then re-arm for the next deadline.
    void OnTimerExpired()
    {
        auto const now = _clock.Now();
        // Route due entries back through the post seam instead of
        // resuming inline: one resume path, and FIFO fairness with
        // continuations posted before the timer fired.
        while (auto const due = _timers.PopDue(now))
            _post(due.value().handle);
        Rearm();
    }

    /// @return Number of timer entries not yet handed back to the pump.
    [[nodiscard]] std::size_t PendingCount() const noexcept
    {
        return _timers.Size();
    }

  private:
    /// Point the host's single timer at the earliest pending deadline, or
    /// cancel it when no timers remain. The delay is `ceil`-rounded to
    /// whole milliseconds and clamped to be non-negative; the host applies
    /// any further platform bound.
    void Rearm()
    {
        auto const next = _timers.NextDeadline();
        if (!next)
        {
            _cancelTimer();
            return;
        }
        auto const now = _clock.Now();
        auto const delay = next.value() > now ? std::chrono::ceil<std::chrono::milliseconds>(next.value() - now)
                                              : std::chrono::milliseconds { 0 };
        _armTimer(delay);
    }

    IClock& _clock;
    TimerQueue _timers;
    PostFn _post;
    ArmTimerFn _armTimer;
    CancelTimerFn _cancelTimer;
};

} // namespace Coro::Detail
