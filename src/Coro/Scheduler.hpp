// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Task.hpp>

#include <coroutine>
#include <utility>

namespace Coro
{

/// Abstract scheduler interface. The awaiters (`Sleep`, `WhenAll`,
/// `WhenAny`, `Spawn`) talk to this — never to a concrete event loop.
/// That seam is what lets tests inject a fake-clock loop and lets the
/// production demos use the real one. (AGENT.md: every I/O / time
/// dependency goes through an interface, never a singleton.)
///
/// Implementations are single-threaded and assume callers run on the
/// loop thread.
class IScheduler
{
  public:
    IScheduler() = default;
    IScheduler(IScheduler const&) = delete;
    IScheduler& operator=(IScheduler const&) = delete;
    IScheduler(IScheduler&&) = delete;
    IScheduler& operator=(IScheduler&&) = delete;
    virtual ~IScheduler() = default;

    /// Post a ready continuation to the scheduler's run queue. The
    /// scheduler will resume `handle` on its next iteration.
    /// @param handle Suspended coroutine to resume.
    virtual void Post(std::coroutine_handle<> handle) = 0;

    /// Detach a `Task<void>` onto this scheduler and drive it to
    /// completion alongside other tasks.
    ///
    /// Ownership of the coroutine frame transfers to the scheduler: the
    /// task is released and its raw handle posted to the run queue. The
    /// coroutine self-destroys at `final_suspend` (its continuation is the
    /// `noop_coroutine`), so the scheduler need not track the frame.
    ///
    /// This is the member-API counterpart to the free function
    /// @ref Spawn, letting callers write `scheduler.Post(task())` instead
    /// of `Coro::Spawn(scheduler, task())`. Restricted to `Task<void>`
    /// on purpose: a detached value-returning task would silently drop its
    /// result, so the type system forbids it.
    ///
    /// @param task Fresh, never-awaited task to start. Must own a frame;
    ///             moved-from on return.
    void Post(Task<void>&& task)
    {
        Post(std::move(task).Release());
    }

    /// Schedule a continuation to be resumed at (or shortly after) the
    /// given time point on the scheduler's clock.
    /// @param when Earliest time at which `handle` may be resumed.
    /// @param handle Suspended coroutine to resume.
    virtual void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) = 0;

    /// @return The clock the scheduler uses for time-based scheduling.
    /// Awaiters read `Now()` from this to compute deadlines for
    /// `ScheduleAt`.
    [[nodiscard]] virtual IClock const& Clock() const noexcept = 0;
};

} // namespace Coro
