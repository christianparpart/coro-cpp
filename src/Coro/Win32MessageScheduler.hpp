// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>
#include <Coro/TimerQueue.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>

namespace Coro
{

namespace Detail
{
    struct Win32MessageSchedulerAccess;
} // namespace Detail

/// `IScheduler` implementation that rides an existing Win32 message pump
/// instead of owning a loop of its own.
///
/// This is the scheduler for GUI applications — MFC, raw Win32, WTL —
/// where the framework already runs `GetMessage`/`DispatchMessage`. The
/// scheduler creates a hidden *message-only* window and maps the
/// `IScheduler` contract onto window messages:
///
///   * `Post(handle)` becomes a private `WM_APP`-range message carrying
///     the coroutine handle; the window procedure resumes it when the
///     application's own pump dispatches the message. Every resume thus
///     runs on the pump (GUI) thread.
///   * `ScheduleAt(when, handle)` parks the handle in the shared
///     @ref TimerQueue and arms a single `SetTimer` slot for the
///     earliest deadline; `WM_TIMER` drains due entries back through
///     `Post` and re-arms.
///
/// A button handler can therefore `Spawn` a `Task<void>` that
/// `co_await`s `Sleep`/`WhenAll` chains while the UI stays responsive —
/// no blocking waits, no hand-rolled `WM_TIMER` state machines.
///
/// @par Threading contract
/// Unlike the other schedulers, `Post` is safe to call from *any*
/// thread (`PostMessage` is inherently thread-safe) — the moral
/// equivalent of "resume on the GUI thread". Everything else —
/// `ScheduleAt`, construction, destruction — must happen on the thread
/// that runs the pump. Coroutines always resume on the pump thread.
///
/// @par Timer precision
/// `WM_TIMER` is a low-priority, coalesced message with roughly
/// 10–15 ms granularity; `Sleep(scheduler, 1ms)` will not be punctual
/// here the way it is on `EventLoop`. Deadlines already due are posted
/// immediately and skip the timer entirely.
///
/// @par Nested pumps and teardown
/// Modal dialogs and menu loops run nested pumps that still dispatch
/// posted messages, so coroutines keep making progress while a dialog
/// is up — be deliberate about mutating document state from them. On
/// destruction, resume messages still queued are *dropped* (their
/// frames stay with their owners), mirroring `EventLoop` dropping
/// unfired timer handles; a *detached* task that never got to run leaks
/// its self-owned frame, so drain the pump before tearing down.
class Win32MessageScheduler final: public IScheduler
{
  public:
    /// Create a scheduler bound to the calling thread's message pump.
    ///
    /// Registers the (process-wide) hidden window class on first use and
    /// creates this instance's message-only window.
    /// @param clock Time source used by `ScheduleAt` deadlines; pass
    ///              `SystemClock` in production. Must outlive the
    ///              scheduler.
    /// @return The scheduler, or the `GetLastError` code translated to a
    ///         `std::error_code` (system category) when window-class
    ///         registration or window creation fails.
    [[nodiscard]] static std::expected<std::unique_ptr<Win32MessageScheduler>, std::error_code> Create(IClock& clock);

    Win32MessageScheduler(Win32MessageScheduler const&) = delete;
    Win32MessageScheduler& operator=(Win32MessageScheduler const&) = delete;
    Win32MessageScheduler(Win32MessageScheduler&&) = delete;
    Win32MessageScheduler& operator=(Win32MessageScheduler&&) = delete;

    /// Destroys the hidden window after killing the timer slot and
    /// dropping any still-queued resume messages (see class notes on
    /// teardown). Must run on the pump thread.
    ~Win32MessageScheduler() override;

    using IScheduler::Post; // keep the Task<void>&& convenience overload visible

    /// Enqueue @p handle for resumption by the pump thread. Callable
    /// from any thread.
    /// @param handle Suspended coroutine to resume on the pump thread.
    void Post(std::coroutine_handle<> handle) override;

    /// Arm @p handle to resume at (or shortly after) @p when. Pump
    /// thread only. Deadlines already due are posted immediately.
    /// @param when Earliest time at which @p handle may be resumed.
    /// @param handle Suspended coroutine to resume.
    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) override;

    [[nodiscard]] IClock const& Clock() const noexcept override;

    /// @return Number of timer entries not yet handed back to the pump.
    [[nodiscard]] std::size_t PendingTimerCount() const noexcept;

  private:
    friend struct Detail::Win32MessageSchedulerAccess;

    /// @param clock Injected time source.
    /// @param window `HWND` of the freshly created message-only window.
    /// @param threadId Win32 id of the pump thread that owns @p window.
    Win32MessageScheduler(IClock& clock, void* window, std::uint32_t threadId) noexcept;

    /// `WM_TIMER` handler: posts every due timer entry back through
    /// `Post` and re-arms for the next deadline.
    void OnTimer();

    /// Point the single `SetTimer` slot at the earliest pending
    /// deadline, or kill it when no timers remain.
    void RearmTimer();

    IClock& _clock;
    void* _window; ///< `HWND` of the hidden message-only window, stored opaquely so this header stays free of <windows.h>.
    std::uint32_t _threadId; ///< Win32 thread id of the pump thread.
    TimerQueue _timers;
};

} // namespace Coro
