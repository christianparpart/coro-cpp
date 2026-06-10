// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <thread>

namespace Coro
{

/// Time source seam for the scheduler. Injected into the event loop so
/// tests can drive a deterministic fake clock instead of real wall time
/// — this is the AGENT.md DI rule applied to "ambient" time.
///
/// Real production code uses `SystemClock`; unit tests construct a
/// `ManualClock` (in tests/ManualClock.hpp) and advance it explicitly.
class IClock
{
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    IClock() = default;
    IClock(IClock const&) = delete;
    IClock& operator=(IClock const&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;
    virtual ~IClock() = default;

    /// @return The current time, in the clock's own time domain.
    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;

    /// Wait until `Now() >= deadline`, in the clock's own time domain.
    ///
    /// This keeps *waiting* behind the same DI seam as *reading* time: the
    /// event loop never calls `std::this_thread::sleep_until` directly,
    /// because that would interpret an injected fake clock's time points
    /// as wall time. `SystemClock` really blocks the thread; a test clock
    /// simply jumps forward (see `tests::ManualClock`), which makes
    /// `EventLoop::Run` deterministic and instant under test.
    /// @param deadline Time point that `Now()` must have reached when this
    ///                 call returns.
    virtual void WaitUntil(TimePoint deadline) = 0;
};

/// Real-time clock backed by `std::chrono::steady_clock`. Default
/// implementation used by the event loop in production.
class SystemClock final: public IClock
{
  public:
    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }

    /// Blocks the calling thread until @p deadline (wall-clock sleep).
    /// @param deadline Steady-clock time point to sleep until.
    void WaitUntil(TimePoint deadline) override
    {
        std::this_thread::sleep_until(deadline);
    }
};

} // namespace Coro
