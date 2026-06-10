// SPDX-License-Identifier: Apache-2.0
//
// Demo 4 (the closer): three async fetches in parallel via WhenAll.
// Same body shape as `demo_sequential_fetch` except the three awaits
// are wrapped in a single `WhenAll`. Total elapsed time should equal
// the slowest single fetch, not the sum.

#include "Fake.hpp"

#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Task.hpp>
#include <Coro/WhenAll.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

namespace
{

Coro::Task<void> Run(Coro::IScheduler& scheduler)
{
    auto [user, posts, stats] = co_await Coro::WhenAll(scheduler,
                                                      demos::Fetch(scheduler, "/user/42", 250ms),
                                                      demos::Fetch(scheduler, "/user/42/posts", 250ms),
                                                      demos::Fetch(scheduler, "/user/42/stats", 250ms));
    std::cout << user << '\n' << posts << '\n' << stats << '\n';
}

} // namespace

int main()
{
    auto clock = Coro::SystemClock {};
    auto loop = Coro::EventLoop { clock };
    auto const start = clock.Now();
    loop.Run(Run(loop));
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start);
    std::cout << "elapsed: " << elapsed.count() << "ms (max of three sleeps)\n";
    return 0;
}
