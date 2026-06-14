// SPDX-License-Identifier: Apache-2.0
#include <Coro/PumpTimerDriver.hpp>
#include <Coro/QtScheduler.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/Qt>

namespace Coro
{

namespace Detail
{

    /// Hidden Qt machinery backing a @ref QtScheduler, kept out of the public
    /// header so consumers need no `<QtCore>`.
    ///
    /// Holds the GUI-thread `QObject` that resume functors are posted to, the
    /// single re-used `QTimer`, and the shared @ref PumpTimerDriver wired to
    /// them. Members are declared in dependency order: `object` first (the
    /// timer is parented to it), then `timer`, then `driver` (its transport
    /// lambdas capture the two above).
    struct QtSchedulerContext
    {
        /// @param clock Time source forwarded to the driver.
        explicit QtSchedulerContext(IClock& clock):
            object {},
            timer { &object },
            driver {
                clock,
                // Resume seam: queue a functor that resumes the handle on the
                // object's (GUI) thread. Safe to call from any thread.
                [this](std::coroutine_handle<> handle) {
                    QMetaObject::invokeMethod(&object, [handle] { handle.resume(); }, Qt::QueuedConnection);
                },
                // Arm seam: (re-)start the single one-shot timer. start()
                // replaces any prior arming, so exactly one timer is live.
                [this](std::chrono::milliseconds delay) { timer.start(delay); },
                // Cancel seam.
                [this] { timer.stop(); },
            }
        {
            timer.setSingleShot(true);
        }

        QObject object;                 ///< GUI-thread receiver for resume functors and the timer.
        QTimer timer;                   ///< Single re-used one-shot timer; parented for thread + lifetime.
        Detail::PumpTimerDriver driver; ///< Shared due/park/re-arm algorithm.
    };

} // namespace Detail

std::expected<std::unique_ptr<QtScheduler>, std::error_code> QtScheduler::Create(IClock& clock)
{
    // The scheduler rides QCoreApplication's event loop; without an
    // application instance there is no loop to post onto.
    if (QCoreApplication::instance() == nullptr)
        return std::unexpected { std::make_error_code(std::errc::operation_not_permitted) };

    auto context = std::make_unique<Detail::QtSchedulerContext>(clock);

    // Drain due timers back through the same resume path as Post (one
    // resume path, FIFO fairness). The context object is the connection's
    // receiver, so the connection auto-severs when the object is destroyed
    // on teardown.
    auto* const driver = &context->driver;
    QObject::connect(&context->timer, &QTimer::timeout, &context->object, [driver] { driver->OnTimerExpired(); });

    // Private constructor — std::make_unique cannot reach it.
    return std::unique_ptr<QtScheduler> { new QtScheduler { clock, std::move(context) } };
}

QtScheduler::QtScheduler(IClock& clock, std::unique_ptr<Detail::QtSchedulerContext> context) noexcept:
    _clock { clock },
    _context { std::move(context) }
{
}

QtScheduler::~QtScheduler()
{
    // Stop the timer, then let the unique_ptr destroy the QObject. Qt
    // discards any resume functors still queued for that object — those
    // continuations are dropped, not resumed (see header teardown notes).
    _context->timer.stop();
}

void QtScheduler::Post(std::coroutine_handle<> handle)
{
    QMetaObject::invokeMethod(&_context->object, [handle] { handle.resume(); }, Qt::QueuedConnection);
}

void QtScheduler::ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle)
{
    // The shared driver owns the due/park/re-arm logic; this class only
    // supplies the Qt transport (see QtSchedulerContext).
    _context->driver.ScheduleAt(when, handle);
}

IClock const& QtScheduler::Clock() const noexcept
{
    return _clock;
}

std::size_t QtScheduler::PendingTimerCount() const noexcept
{
    return _context->driver.PendingCount();
}

} // namespace Coro
