// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>

#include <coroutine>

namespace Coro
{

/// Awaitable that suspends the current coroutine for the given duration.
///
/// On `await_suspend` it asks the scheduler to resume the continuation
/// at `now + duration`. Returns nothing on resume. The scheduler is
/// passed by reference because awaiters MUST go through the seam — no
/// global "current loop", no thread-local sleight of hand. (AGENT.md
/// DI rule.)
///
/// Usage:
/// @code
/// co_await Coro::Sleep(scheduler, 100ms);
/// @endcode
class SleepAwaitable
{
  public:
    /// @param scheduler Scheduler that will resume the continuation.
    /// @param duration How long to suspend, in steady-clock units.
    SleepAwaitable(IScheduler& scheduler, IClock::Duration duration) noexcept:
        _scheduler { scheduler },
        _deadline { scheduler.Clock().Now() + duration }
    {
    }

    [[nodiscard]] bool await_ready() const noexcept
    {
        return _deadline <= _scheduler.Clock().Now();
    }

    void await_suspend(std::coroutine_handle<> continuation) noexcept
    {
        _scheduler.ScheduleAt(_deadline, continuation);
    }

    void await_resume() const noexcept {}

  private:
    IScheduler& _scheduler;
    IClock::TimePoint _deadline;
};

/// Convenience factory matching the style `co_await Sleep(scheduler, d)`.
/// @param scheduler Scheduler that will resume the continuation.
/// @param duration How long to suspend.
/// @return A `SleepAwaitable` ready to be awaited.
[[nodiscard]] inline SleepAwaitable Sleep(IScheduler& scheduler, IClock::Duration duration) noexcept
{
    return SleepAwaitable { scheduler, duration };
}

} // namespace Coro
