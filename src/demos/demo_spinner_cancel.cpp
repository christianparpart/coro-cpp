// SPDX-License-Identifier: Apache-2.0
//
// Demo 5 (backup / Q&A): a braille spinner cancelled mid-await via
// std::stop_token. The animation frames are data (an array of glyphs),
// per the project's data-driven-design rule — swapping spinner styles
// is a one-line change to the table, not a code change.

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Spawn.hpp>
#include <Coro/Task.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <ranges>
#include <stop_token>
#include <string_view>

using namespace std::chrono_literals;

namespace
{

constexpr std::array<std::string_view, 10> SpinnerFrames {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};
constexpr auto FrameInterval = 80ms;
constexpr auto WorkDuration = 1500ms;

Coro::Task<void> Spinner(Coro::IScheduler& scheduler, std::stop_token token)
{
    auto i = 0U;
    while (!token.stop_requested())
    {
        std::cout << '\r' << SpinnerFrames[i % SpinnerFrames.size()] << " working..." << std::flush;
        ++i;
        co_await Coro::Sleep(scheduler, FrameInterval);
    }
    std::cout << "\r✔ done.        \n";
}

Coro::Task<void> Work(Coro::IScheduler& scheduler, std::stop_source source)
{
    co_await Coro::Sleep(scheduler, WorkDuration);
    source.request_stop();
}

Coro::Task<void> Run(Coro::IScheduler& scheduler)
{
    auto source = std::stop_source {};
    Coro::Spawn(scheduler, Spinner(scheduler, source.get_token()));
    co_await Work(scheduler, source);
    // Yield once so the spinner observes the stop and prints "done.".
    co_await Coro::Sleep(scheduler, FrameInterval);
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    loop.Run(Run(loop));
    return 0;
}
