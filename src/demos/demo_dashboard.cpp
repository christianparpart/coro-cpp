// SPDX-License-Identifier: Apache-2.0
//
// Demo 6: two independent screen regions animate "simultaneously" until
// the user presses any key. Each region is driven by its OWN coroutine
// written in a flat, sequential style — a `while` loop that prints a
// frame and `co_await`s a Sleep. There is no thread, no callback, no
// state machine: the single-threaded event loop interleaves the two
// coroutines so both regions appear to update at once.
//
// The whole demo is data-driven (AGENT.md): each animated region is a
// row in `Regions`, carrying its screen line, frame table and tick
// interval. Adding a third spinner is a new row, not new code. Input is
// reached through the injected `IKeyPress` seam, never a raw `::read()`.

#include "KeyPress.hpp"

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Spawn.hpp>
#include <Coro/Task.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <span>
#include <stop_token>
#include <string_view>

using namespace std::chrono_literals;

namespace
{

/// Static description of one animated screen region. Behaviour is data:
/// the animation, where it is drawn, and how fast it ticks all live
/// here, so the loop in `Animate` interprets the descriptor rather than
/// hard-coding any single region.
struct Region
{
    int line { 0 };                             ///< 1-based terminal row to draw on.
    std::string_view label;                     ///< Text printed after the glyph.
    std::span<std::string_view const> frames;   ///< Animation glyphs, cycled in order.
    std::chrono::milliseconds interval { 0ms }; ///< Delay between frames.
};

constexpr std::array<std::string_view, 10> BrailleFrames { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
constexpr std::array<std::string_view, 4> ClockFrames { "◐", "◓", "◑", "◒" };

/// The two regions that animate at once. A third would be one more row.
constexpr std::array<Region, 2> Regions {
    Region { .line = 2, .label = "fetching data", .frames = BrailleFrames, .interval = 90ms },
    Region { .line = 3, .label = "syncing cache", .frames = ClockFrames, .interval = 160ms },
};

/// How often the keypress poller wakes to check the injected probe.
constexpr auto PollInterval = 30ms;

/// RAII guard that hides the terminal cursor for its lifetime and shows
/// it again on destruction (even on an exception or early return). With
/// the cursor hidden, the rapid `CursorTo` jumps between regions no
/// longer make a visible caret flicker across the screen.
class CursorHider
{
  public:
    /// Emit the "hide cursor" control sequence.
    CursorHider()
    {
        std::cout << "\x1b[?25l" << std::flush;
    }

    CursorHider(CursorHider const&) = delete;
    CursorHider& operator=(CursorHider const&) = delete;
    CursorHider(CursorHider&&) = delete;
    CursorHider& operator=(CursorHider&&) = delete;

    /// Emit the "show cursor" control sequence, restoring normal display.
    ~CursorHider()
    {
        std::cout << "\x1b[?25h" << std::flush;
    }
};

/// Move the cursor to (row, col=1) and clear the line. ANSI control
/// sequences are the data here; the helper keeps the escape soup in one
/// place so the animation coroutines stay readable.
/// @param line 1-based terminal row.
/// @return The escape sequence positioning the cursor at the line start.
std::string CursorTo(int line, int column = 1)
{
    return std::format("\x1b[{};{}H\x1b[2K", line, column);
}

/// Animate a single region until cancellation is requested.
///
/// Written to *read* sequentially — print a frame, sleep, repeat — even
/// though it is suspended and resumed cooperatively between every
/// `co_await`. While this coroutine sleeps, its sibling region's
/// coroutine runs, which is what makes both regions appear to update
/// simultaneously on a single thread.
///
/// @param scheduler Scheduler driving the cooperative sleeps.
/// @param region The region descriptor to render.
/// @param token Stop token; the loop exits once a key has been pressed.
Coro::Task<void> Animate(Coro::IScheduler& scheduler, Region region, std::stop_token token)
{
    auto frame = std::size_t { 0 };
    while (!token.stop_requested())
    {
        std::cout << CursorTo(region.line) << region.frames[frame % region.frames.size()] << ' ' << region.label
                  << std::flush;
        ++frame;
        co_await Coro::Sleep(scheduler, region.interval);
    }
    std::cout << CursorTo(region.line) << "✔ " << region.label << " — stopped" << std::flush;
}

/// Poll the injected keypress probe until a key arrives, then request a
/// stop so every region coroutine unwinds on its next tick.
///
/// @param scheduler Scheduler driving the poll cadence.
/// @param keys Injected, non-blocking keyboard probe.
/// @param source Stop source shared with the region coroutines.
Coro::Task<void> WaitForKey(Coro::IScheduler& scheduler, demos::IKeyPress& keys, std::stop_source source)
{
    while (!keys.Pressed())
        co_await Coro::Sleep(scheduler, PollInterval);
    source.request_stop();
}

/// Wire the demo together: spawn one coroutine per region plus the key
/// poller, all sharing a single stop source, and run until a keypress.
///
/// @param scheduler Scheduler that interleaves every coroutine.
/// @param keys Injected keyboard probe deciding when to stop.
Coro::Task<void> Run(Coro::IScheduler& scheduler, demos::IKeyPress& keys)
{
    auto source = std::stop_source {};

    std::cout << "\x1b[2J\x1b[1;1HPress any key to stop the dashboard.\n" << std::flush;

    // Each region is its own self-contained coroutine; the loop drives
    // them concurrently without threads.
    for (auto const& region: Regions)
        Coro::Spawn(scheduler, Animate(scheduler, region, source.get_token()));

    // Block the root on the key poller. When it returns, the stop has
    // been requested; the event loop then drains the detached region
    // coroutines, so each one wakes on its next tick, observes the stop,
    // and prints its final "stopped" frame before `EventLoop::Run`
    // returns — no matter how its interval relates to the poll cadence.
    co_await WaitForKey(scheduler, keys, source);
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    auto keys = demos::TerminalKeyPress {};
    auto const cursor = CursorHider {};
    loop.Run(Run(loop, keys));
    // Run has drained every region coroutine (each printed its final
    // "stopped" frame), so the dashboard is complete — park the cursor
    // below it before handing the terminal back.
    std::cout << CursorTo(static_cast<int>(Regions.size()) + 4) << '\n' << std::flush;
    return 0;
}
