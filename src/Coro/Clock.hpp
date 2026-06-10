// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

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
};

} // namespace Coro
