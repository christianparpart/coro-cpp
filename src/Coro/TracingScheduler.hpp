// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Coro
{

/// One recorded scheduler interaction (see @ref TracingScheduler).
struct TraceEvent
{
    /// Which `IScheduler` operation was invoked.
    enum class Kind : std::uint8_t
    {
        Post,       ///< `Post(handle)` — ready continuation enqueued.
        ScheduleAt, ///< `ScheduleAt(when, handle)` — timer armed.
    };

    Kind kind;                      ///< The operation recorded.
    IClock::TimePoint at;           ///< Clock time when the call was made.
    IClock::TimePoint when;         ///< Requested resume time (== @ref at for `Post`).
    std::coroutine_handle<> handle; ///< The continuation passed (opaque identity, never resumed by the trace).
};

/// Display names for @ref TraceEvent::Kind — the single source of truth,
/// indexed by the enumerator's underlying value.
inline constexpr auto TraceEventKindNames = std::array<std::string_view, 2> { "Post", "ScheduleAt" };

/// @param kind The event kind to name.
/// @return Human-readable name of @p kind (e.g. for demo log lines).
[[nodiscard]] constexpr std::string_view TraceEventKindName(TraceEvent::Kind kind) noexcept
{
    return TraceEventKindNames.at(std::to_underlying(kind));
}

/// Decorator `IScheduler` that records every `Post`/`ScheduleAt` call
/// before forwarding it to the wrapped scheduler.
///
/// Two consumers, two channels:
///   * tests assert on the in-memory recording (@ref Events,
///     @ref Count) — "exactly one timer was armed, at t+50ms" — instead
///     of inferring scheduling behaviour from task side effects;
///   * demos inject a @ref Sink that prints events live, making the
///     machinery behind `WhenAll`/`Sleep` visible to the audience.
///
/// Pure decorator: timing, ordering, and clock all come from the inner
/// scheduler, which must outlive this wrapper. Recorded handles are
/// opaque identities — the trace never resumes or destroys them.
class TracingScheduler final: public IScheduler
{
  public:
    /// Callback observing each event synchronously, inside the
    /// `Post`/`ScheduleAt` call that triggered it.
    using Sink = std::function<void(TraceEvent const&)>;

    /// Wrap @p inner, recording every scheduling call.
    /// @param inner Scheduler that performs the real work. Must outlive
    ///              this wrapper.
    /// @param sink Optional observer invoked per event after it has been
    ///             recorded (e.g. a demo's log printer).
    explicit TracingScheduler(IScheduler& inner, Sink sink = {}):
        _inner { inner },
        _sink { std::move(sink) }
    {
    }

    using IScheduler::Post; // keep the Task<void>&& convenience overload visible

    void Post(std::coroutine_handle<> handle) override
    {
        auto const now = _inner.Clock().Now();
        Record(TraceEvent { .kind = TraceEvent::Kind::Post, .at = now, .when = now, .handle = handle });
        _inner.Post(handle);
    }

    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) override
    {
        Record(
            TraceEvent { .kind = TraceEvent::Kind::ScheduleAt, .at = _inner.Clock().Now(), .when = when, .handle = handle });
        _inner.ScheduleAt(when, handle);
    }

    [[nodiscard]] IClock const& Clock() const noexcept override
    {
        return _inner.Clock();
    }

    /// @return All events recorded so far, in call order.
    [[nodiscard]] std::span<TraceEvent const> Events() const noexcept
    {
        return _events;
    }

    /// @param kind The event kind to count.
    /// @return Number of recorded events of the given kind.
    [[nodiscard]] std::size_t Count(TraceEvent::Kind kind) const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count(_events, kind, &TraceEvent::kind));
    }

    /// Discard the recording (the sink is unaffected).
    void Clear() noexcept
    {
        _events.clear();
    }

  private:
    /// Append @p event to the recording and notify the sink, if any.
    void Record(TraceEvent const& event)
    {
        _events.push_back(event);
        if (_sink)
            _sink(event);
    }

    IScheduler& _inner;
    Sink _sink;
    std::vector<TraceEvent> _events;
};

} // namespace Coro
