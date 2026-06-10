// SPDX-License-Identifier: Apache-2.0
//
// Demo 1 (the opener). The point is visual: a function appears to
// suspend and resume between characters, but nothing on the call stack
// is blocking — the event loop is running other work (none here, but
// the seam is the same as `demo_when_all`).

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <chrono>
#include <iostream>
#include <string_view>

using namespace std::chrono_literals;

namespace
{

/// Print `text` one character at a time, suspending between each.
Coro::Task<void> TypeOut(Coro::IScheduler& scheduler, std::string_view text)
{
    for (auto c: text)
    {
        std::cout << c << std::flush;
        co_await Coro::Sleep(scheduler, 40ms);
    }
    std::cout << '\n';
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    loop.Run(TypeOut(loop, "Hello from a C++23 coroutine."));
    return 0;
}
