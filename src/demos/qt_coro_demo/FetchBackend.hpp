// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Clock.hpp>
#include <Coro/QtScheduler.hpp>
#include <Coro/Task.hpp>

#include <memory>
#include <stop_token>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtQml/qqmlregistration.h>

namespace demos
{

/// QML-facing backend that runs a sequence of simulated network fetches as
/// straight-line coroutine code, driving the UI through `Q_PROPERTY`s.
///
/// This is the whole point of the demo: @ref start kicks off three
/// sequential fetches written as a linear `co_await` script (see
/// @ref RunSequence), yet the GUI thread is never blocked — the QML
/// `BusyIndicator` keeps spinning and the window stays responsive. The same
/// logic with `QFutureWatcher`/signal-slot callbacks would be a pyramid of
/// nested handlers, and cancelling it midway would mean tracking which
/// stage is in flight and tearing it down by hand. Here cancellation is a
/// single `std::stop_token` check the linear code already reads
/// top-to-bottom (@ref cancel).
///
/// The scheduler is injected at construction via @ref Coro::QtScheduler,
/// which rides the application's Qt event loop (AGENT.md DI rule: the
/// coroutine reaches time/IO only through that seam).
class FetchBackend: public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int step READ step NOTIFY stepChanged)
    Q_PROPERTY(int stepCount READ stepCount CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)

  public:
    /// @param parent Optional QObject parent (QML sets this).
    explicit FetchBackend(QObject* parent = nullptr);

    FetchBackend(FetchBackend const&) = delete;
    FetchBackend& operator=(FetchBackend const&) = delete;
    FetchBackend(FetchBackend&&) = delete;
    FetchBackend& operator=(FetchBackend&&) = delete;

    ~FetchBackend() override;

    /// @return Human-readable status of the current/last sequence.
    [[nodiscard]] QString status() const
    {
        return _status;
    }

    /// @return Number of fetch steps completed so far (0..stepCount).
    [[nodiscard]] int step() const
    {
        return _step;
    }

    /// @return Total number of fetch steps in a full sequence (for the
    ///         progress bar's range). Data-driven — equals the step table
    ///         size.
    [[nodiscard]] int stepCount() const;

    /// @return Whether a fetch sequence is currently in flight.
    [[nodiscard]] bool busy() const
    {
        return _busy;
    }

    /// @return The combined payload once the sequence finishes.
    [[nodiscard]] QString result() const
    {
        return _result;
    }

    /// Start the fetch sequence. No-op while a sequence is already running.
    /// Spawns @ref RunSequence onto the Qt-event-loop scheduler.
    Q_INVOKABLE void start();

    /// Request cancellation of the in-flight sequence. The running
    /// coroutine observes it at its next step boundary and unwinds.
    Q_INVOKABLE void cancel();

  signals:
    /// Emitted when @ref status changes.
    void statusChanged();
    /// Emitted when @ref step changes.
    void stepChanged();
    /// Emitted when @ref busy changes.
    void busyChanged();
    /// Emitted when @ref result changes.
    void resultChanged();

  private:
    /// The linear async script: walk the fetch step table, `co_await`ing
    /// each simulated fetch and updating the QML-visible state between
    /// steps. Checks @p token at each boundary so @ref cancel can abort it.
    /// @param token Cancellation token tied to the current sequence.
    /// @return A detached task driven by the scheduler.
    Coro::Task<void> RunSequence(std::stop_token token);

    /// @name Property setters (emit the matching ...Changed only on change).
    /// @{
    void setStatus(QString value);
    void setStep(int value);
    void setBusy(bool value);
    void setResult(QString value);
    /// @}

    QString _status { "Idle." }; ///< Current status line.
    int _step { 0 };             ///< Completed steps in the running sequence.
    bool _busy { false };        ///< Whether a sequence is in flight.
    QString _result {};          ///< Combined payload of the last run.

    Coro::SystemClock _clock {};                   ///< Real-time source for the scheduler.
    std::unique_ptr<Coro::QtScheduler> _scheduler; ///< Rides the Qt event loop.
    std::stop_source _source {};                   ///< Cancels the in-flight sequence.
};

} // namespace demos
