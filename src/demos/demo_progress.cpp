// SPDX-License-Identifier: Apache-2.0
//
// Demo 2: a progress bar driven by a plain `for` loop. The audience
// sees the bar fill in real time. Equivalent Qt code would be a
// QTimer-driven slot tracking the index in a member field; here the
// state lives on the coroutine frame and reads top-to-bottom.

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <chrono>
#include <iostream>
#include <ranges>
#include <string>

using namespace std::chrono_literals;

namespace
{

constexpr int BarWidth = 50;
constexpr int TotalSteps = 100;
constexpr auto StepInterval = 30ms;

Coro::Task<void> RunProgress(Coro::IScheduler& scheduler)
{
    for (auto i: std::views::iota(0, TotalSteps + 1))
    {
        auto const filled = (i * BarWidth) / TotalSteps;
        std::cout << '\r' << '[' << std::string(static_cast<std::size_t>(filled), '#')
                  << std::string(static_cast<std::size_t>(BarWidth - filled), ' ') << "] " << i << '%' << std::flush;
        co_await Coro::Sleep(scheduler, StepInterval);
    }
    std::cout << "\ndone.\n";
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    loop.Run(RunProgress(loop));
    return 0;
}
