// SPDX-License-Identifier: Apache-2.0
#include "FetchBackend.hpp"

#include <Coro/Sleep.hpp>
#include <Coro/Spawn.hpp>

#include <array>
#include <chrono>
#include <stop_token>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace demos
{

namespace
{

    /// One step of the simulated fetch sequence. Data-driven design: the
    /// sequence is a table the coroutine iterates, so adding or reordering
    /// a step is a new row here — not a new branch in the logic.
    struct FetchStep
    {
        std::string_view status;           ///< Status shown while this step runs.
        std::string_view path;             ///< Endpoint we pretend to fetch.
        std::chrono::milliseconds latency; ///< Simulated round-trip time.
    };

    /// The sequence: log in, then load the profile, then the posts. Latency
    /// is deliberately long (~2s each, ~6s total) so an audience has time to
    /// watch each step: the spinner keeps spinning and the window stays
    /// draggable while the progress bar advances, and there is a comfortable
    /// window to hit Cancel mid-flight.
    constexpr std::array<FetchStep, 3> FetchSteps {
        FetchStep { .status = "Logging in…", .path = "/auth/login", .latency = 2000ms },
        FetchStep { .status = "Fetching profile…", .path = "/user/42", .latency = 2000ms },
        FetchStep { .status = "Fetching posts…", .path = "/user/42/posts", .latency = 2000ms },
    };

    /// Simulated HTTP GET: suspends for @p latency, then returns a canned
    /// payload (the demo has no real network — production code injects a
    /// real client through its own interface).
    /// @param scheduler Scheduler used by `Sleep`.
    /// @param path Endpoint label echoed into the reply.
    /// @param latency Simulated round-trip time.
    /// @return The fake reply for @p path.
    Coro::Task<QString> Fetch(Coro::IScheduler& scheduler, std::string_view path, std::chrono::milliseconds latency)
    {
        co_await Coro::Sleep(scheduler, latency);
        co_return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())) + " → 200 OK";
    }

} // namespace

FetchBackend::FetchBackend(QObject* parent):
    QObject { parent }
{
    // The scheduler rides the already-running Qt event loop. Create()
    // requires a QCoreApplication to exist, which it does by the time QML
    // instantiates this element.
    auto scheduler = Coro::QtScheduler::Create(_clock);
    Q_ASSERT_X(scheduler.has_value(), "FetchBackend", "QtScheduler::Create requires a running QCoreApplication");
    _scheduler = std::move(scheduler).value();
}

FetchBackend::~FetchBackend()
{
    // Abort any in-flight sequence before the scheduler (and this backend)
    // are torn down, so the coroutine stops touching us.
    _source.request_stop();
}

int FetchBackend::stepCount() const
{
    return static_cast<int>(FetchSteps.size());
}

void FetchBackend::start()
{
    if (_busy)
        return; // a sequence is already running

    // Fresh cancellation channel for this run.
    _source = std::stop_source {};
    setResult({});
    setStep(0);
    setBusy(true);
    Coro::Spawn(*_scheduler, RunSequence(_source.get_token()));
}

void FetchBackend::cancel()
{
    if (_busy)
        _source.request_stop();
}

Coro::Task<void> FetchBackend::RunSequence(std::stop_token token)
{
    // The money slide: a linear script. Each iteration awaits a simulated
    // fetch; the UI updates between awaits; the GUI thread is never blocked.
    auto combined = QString {};
    auto stepIndex = 0;
    for (auto const& step: FetchSteps)
    {
        if (token.stop_requested())
        {
            setStatus("Cancelled.");
            setBusy(false);
            co_return;
        }

        setStatus(QString::fromUtf8(step.status.data(), static_cast<qsizetype>(step.status.size())));
        auto reply = co_await Fetch(*_scheduler, step.path, step.latency);

        if (!combined.isEmpty())
            combined += '\n';
        combined += reply;

        setStep(++stepIndex);
    }

    // One final cancellation check, in case the stop landed during the last
    // await.
    if (token.stop_requested())
    {
        setStatus("Cancelled.");
        setBusy(false);
        co_return;
    }

    setResult(combined);
    setStatus("Done.");
    setBusy(false);
}

void FetchBackend::setStatus(QString value)
{
    if (_status == value)
        return;
    _status = std::move(value);
    emit statusChanged();
}

void FetchBackend::setStep(int value)
{
    if (_step == value)
        return;
    _step = value;
    emit stepChanged();
}

void FetchBackend::setBusy(bool value)
{
    if (_busy == value)
        return;
    _busy = value;
    emit busyChanged();
}

void FetchBackend::setResult(QString value)
{
    if (_result == value)
        return;
    _result = std::move(value);
    emit resultChanged();
}

} // namespace demos
