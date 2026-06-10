// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <vector>

namespace Coro
{

/// Min-heap of (deadline, coroutine handle) timer entries with FIFO
/// tiebreak for entries that share a deadline.
///
/// This is the single source of truth for "which continuation is due
/// next" — every scheduler that supports `IScheduler::ScheduleAt`
/// (`EventLoop`, `ManualScheduler`, `Win32MessageScheduler`) composes
/// this queue instead of hand-rolling its own heap, so deadline ordering
/// and tiebreak semantics are defined exactly once.
///
/// Handles stored here are *borrowed*: the queue never resumes or
/// destroys them. Ownership stays with whoever scheduled the timer
/// (typically a suspended awaiter's `Task`).
class TimerQueue
{
  public:
    /// A due timer popped from the queue.
    struct Entry
    {
        IClock::TimePoint when;         ///< The deadline the entry was scheduled for.
        std::coroutine_handle<> handle; ///< The continuation to resume.
    };

    /// Insert a timer entry.
    /// @param when Deadline at which @p handle becomes due.
    /// @param handle Continuation to surface once @p when has elapsed.
    void Schedule(IClock::TimePoint when, std::coroutine_handle<> handle)
    {
        _entries.push_back(HeapEntry { .when = when, .handle = handle, .sequence = _nextSequence++ });
        std::ranges::push_heap(_entries, Later {});
    }

    /// Pop the earliest entry whose deadline has elapsed.
    /// @param now The current time on the owning scheduler's clock.
    /// @return The earliest due entry, or `std::nullopt` if the queue is
    ///         empty or the earliest deadline is still in the future.
    [[nodiscard]] std::optional<Entry> PopDue(IClock::TimePoint now)
    {
        if (_entries.empty() || _entries.front().when > now)
            return std::nullopt;
        std::ranges::pop_heap(_entries, Later {});
        auto const entry = _entries.back();
        _entries.pop_back();
        return Entry { .when = entry.when, .handle = entry.handle };
    }

    /// @return The earliest pending deadline, or `std::nullopt` if the
    ///         queue is empty.
    [[nodiscard]] std::optional<IClock::TimePoint> NextDeadline() const noexcept
    {
        if (_entries.empty())
            return std::nullopt;
        return _entries.front().when;
    }

    /// @return `true` if no timers are pending.
    [[nodiscard]] bool Empty() const noexcept
    {
        return _entries.empty();
    }

    /// @return Number of pending timer entries.
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return _entries.size();
    }

  private:
    /// Internal heap entry; carries an insertion sequence number so that
    /// entries with equal deadlines pop in FIFO order.
    struct HeapEntry
    {
        IClock::TimePoint when;
        std::coroutine_handle<> handle;
        std::size_t sequence; ///< FIFO tiebreak for same-deadline entries.
    };

    /// Strict-weak ordering for the min-heap (latest first, so
    /// `std::pop_heap` yields the earliest deadline).
    struct Later
    {
        [[nodiscard]] bool operator()(HeapEntry const& a, HeapEntry const& b) const noexcept
        {
            if (a.when != b.when)
                return a.when > b.when;
            return a.sequence > b.sequence;
        }
    };

    std::vector<HeapEntry> _entries;
    std::size_t _nextSequence { 0 };
};

} // namespace Coro
