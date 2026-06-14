// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/Scheduler.hpp>

#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <system_error>

namespace Coro
{

namespace Detail
{
    struct QtSchedulerContext;
} // namespace Detail

/// `IScheduler` implementation that rides an existing Qt event loop
/// instead of owning a loop of its own.
///
/// This is the scheduler for Qt applications — Qt Widgets, Qt Quick/QML —
/// where `QCoreApplication::exec()` is already running the event loop. The
/// scheduler owns a hidden `QObject` living on the GUI thread and maps the
/// `IScheduler` contract onto Qt's queued-event machinery:
///
///   * `Post(handle)` becomes a `Qt::QueuedConnection`
///     `QMetaObject::invokeMethod` targeting that object; Qt dispatches the
///     functor — resuming the coroutine — from the event loop on the
///     object's (GUI) thread. Every resume thus runs on the GUI thread.
///   * `ScheduleAt(when, handle)` is delegated to the shared
///     @ref Detail::PumpTimerDriver, which parks the handle and arms a
///     single re-used `QTimer` for the earliest deadline; the timer's
///     `timeout` drains due entries back through `Post` and re-arms. The
///     timer logic is identical to @ref Win32MessageScheduler — both
///     schedulers share that one driver rather than re-implementing it.
///
/// A QML button handler can therefore `Spawn` a `Task<void>` that
/// `co_await`s `Sleep`/`WhenAll` chains while the UI stays responsive — no
/// blocking waits, no nested `QFutureWatcher` callback chains.
///
/// @par Threading contract
/// Like @ref Win32MessageScheduler, `Post` is safe to call from *any*
/// thread (Qt marshals a queued `invokeMethod` onto the target object's
/// thread). Everything else — `ScheduleAt`, construction (@ref Create),
/// destruction — must happen on the GUI thread that runs the event loop.
/// Coroutines always resume on the GUI thread.
///
/// @par Timer precision
/// `QTimer` granularity depends on the platform timer; `Sleep(scheduler,
/// 1ms)` is best-effort, not punctual. Deadlines already due are posted
/// immediately and skip the timer entirely.
///
/// @par Teardown
/// On destruction the `QTimer` is stopped and the hidden `QObject` is
/// destroyed, which discards any resume functors Qt still has queued for
/// it — those continuations are *dropped*, not resumed (their frames stay
/// with their owners), mirroring @ref Win32MessageScheduler and
/// `EventLoop` teardown. A *detached* task that never got to run leaks its
/// self-owned frame, so let the event loop settle before tearing down.
class QtScheduler final: public IScheduler
{
  public:
    /// Create a scheduler bound to the running Qt event loop on the
    /// calling (GUI) thread.
    /// @param clock Time source used by `ScheduleAt` deadlines; pass
    ///              `SystemClock` in production. Must outlive the
    ///              scheduler.
    /// @return The scheduler, or `std::errc::operation_not_permitted` when
    ///         no `QCoreApplication` instance exists yet (the event loop
    ///         the scheduler rides has not been created).
    [[nodiscard]] static std::expected<std::unique_ptr<QtScheduler>, std::error_code> Create(IClock& clock);

    QtScheduler(QtScheduler const&) = delete;
    QtScheduler& operator=(QtScheduler const&) = delete;
    QtScheduler(QtScheduler&&) = delete;
    QtScheduler& operator=(QtScheduler&&) = delete;

    /// Stops the timer and destroys the hidden `QObject`, dropping any
    /// still-queued resume functors (see class notes on teardown). Must
    /// run on the GUI thread. Declared out-of-line so the pimpl context
    /// type can stay incomplete in this (Qt-free) header.
    ~QtScheduler() override;

    using IScheduler::Post; // keep the Task<void>&& convenience overload visible

    /// Enqueue @p handle for resumption by the GUI thread's event loop.
    /// Callable from any thread.
    /// @param handle Suspended coroutine to resume on the GUI thread.
    void Post(std::coroutine_handle<> handle) override;

    /// Arm @p handle to resume at (or shortly after) @p when. GUI thread
    /// only. Deadlines already due are posted immediately.
    /// @param when Earliest time at which @p handle may be resumed.
    /// @param handle Suspended coroutine to resume.
    void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) override;

    [[nodiscard]] IClock const& Clock() const noexcept override;

    /// @return Number of timer entries not yet handed back to the event
    ///         loop.
    [[nodiscard]] std::size_t PendingTimerCount() const noexcept;

  private:
    /// @param clock Injected time source.
    /// @param context Pimpl holding the hidden `QObject`, its `QTimer`, and
    ///                the shared timer driver. Created by @ref Create.
    QtScheduler(IClock& clock, std::unique_ptr<Detail::QtSchedulerContext> context) noexcept;

    IClock& _clock;

    /// Opaque pimpl: owns the `QObject` + `QTimer` + `PumpTimerDriver`.
    /// Held by `unique_ptr` so this header stays free of `<QtCore>`; the
    /// out-of-line destructor runs once the type is complete in the `.cpp`.
    std::unique_ptr<Detail::QtSchedulerContext> _context;
};

} // namespace Coro
