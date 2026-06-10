// SPDX-License-Identifier: Apache-2.0
//
// Demo-only helper: simulates a network fetch by sleeping for a
// configurable duration and returning a hard-coded reply. Lives in the
// demos/ folder because it has no business polluting the public Coro
// library — production code injects a real HTTP client through its
// own interface.

#pragma once

#include <Coro/Scheduler.hpp>
#include <Coro/Sleep.hpp>
#include <Coro/Task.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace demos
{

/// Simulated HTTP GET: suspends for `latency`, then returns the canned
/// payload. The latency is per-call so the talk can show overlap with
/// WhenAll (each call has a different deadline).
///
/// @param scheduler Scheduler used by `Sleep`.
/// @param path What endpoint we pretend to fetch — used only for the
///             returned payload string.
/// @param latency Simulated round-trip time.
/// @return The fake reply, `"{path} -> reply"`.
inline Coro::Task<std::string> Fetch(Coro::IScheduler& scheduler,
                                     std::string_view path,
                                     std::chrono::milliseconds latency)
{
    co_await Coro::Sleep(scheduler, latency);
    co_return std::string { path } + " -> reply";
}

} // namespace demos
