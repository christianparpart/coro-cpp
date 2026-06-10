// SPDX-License-Identifier: Apache-2.0
//
// Demo 3 (the money slide): three async fetches in sequence. The
// "before" picture the audience holds in their head is three nested
// lambdas. The "after" is three lines of straight-line code that read
// like a synchronous script.

#include "Fake.hpp"

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Task.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

namespace
{

Coro::Task<void> Run(Coro::IScheduler& scheduler)
{
    auto user = co_await demos::Fetch(scheduler, "/user/42", 250ms);
    std::cout << user << '\n';

    auto posts = co_await demos::Fetch(scheduler, "/user/42/posts", 250ms);
    std::cout << posts << '\n';

    auto stats = co_await demos::Fetch(scheduler, "/user/42/stats", 250ms);
    std::cout << stats << '\n';
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    auto const start = clock.Now();
    loop.Run(Run(loop));
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start);
    std::cout << "elapsed: " << elapsed.count() << "ms (sum of three sleeps)\n";
    return 0;
}
