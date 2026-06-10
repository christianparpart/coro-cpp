// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>

#include <algorithm>

namespace tests
{

/// Test-only `IClock` whose time only advances when callers say so.
/// Used by the unit tests to make the event loop deterministic — no
/// wall-clock sleeps, no flakiness from a busy CI box. The seam this
/// plugs into is the project's DI rule: production code passes a
/// `SystemClock`, tests pass this.
class ManualClock final: public Coro::IClock
{
  public:
    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return _now;
    }

    /// Move the clock forward by `delta`. Tests interleave `Advance`
    /// calls with `EventLoop::RunOnce` to drive timer-based awaiters
    /// past their deadlines step by step.
    void Advance(Duration delta) noexcept
    {
        _now += delta;
    }

    /// "Waits" by jumping straight to @p deadline (never backwards).
    /// This is what lets `EventLoop::Run` complete timer waits
    /// deterministically and instantly under a manual clock.
    /// @param deadline Time point to advance the clock to.
    void WaitUntil(TimePoint deadline) noexcept override
    {
        _now = std::max(_now, deadline);
    }

  private:
    TimePoint _now { TimePoint {} };
};

} // namespace tests
