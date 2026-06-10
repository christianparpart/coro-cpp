// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Scheduler.hpp>
#include <Coro/Task.hpp>

#include <utility>

namespace Coro
{

/// Post the given task to a scheduler and detach: the scheduler will
/// drive it to completion alongside other tasks. The task's coroutine
/// frame is owned by the scheduler from this call onwards (the coroutine
/// self-destroys at `final_suspend` because the continuation is
/// `noop_coroutine`).
///
/// This is the seam the demos use to start a "background" coroutine
/// (e.g. the spinner in `demo_spinner_cancel`) while another task does
/// real work in the foreground. It deliberately takes only a
/// `Task<void>`: `Task` is lazy and would not run unless awaited or
/// posted, and a value-returning task would have its result dropped.
///
/// Thin alias for `scheduler.Post(std::move(task))` — prefer the member
/// form (@ref IScheduler::Post) in new code; this free function is kept
/// for readability and backwards compatibility.
///
/// @param scheduler Scheduler to post the task to.
/// @param task Task to start. Must be a fresh, never-awaited task.
inline void Spawn(IScheduler& scheduler, Task<void>&& task)
{
    scheduler.Post(std::move(task));
}

} // namespace Coro
