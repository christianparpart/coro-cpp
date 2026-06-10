# Coro — an easy-to-use C++23 coroutines library

`Coro` is a small, dependency-free library that turns the raw C++23
coroutine *language constructs* (`co_await`, `co_return`, promise types,
coroutine handles) into a handful of building blocks you can actually use:

- **`Task<T>`** — the return type that makes any function a coroutine.
- **Awaitables** — the things you `co_await` (`Sleep`, `WhenAll`, `WhenAny`,
  or your own).
- **`IScheduler` / `EventLoop`** — the cooperative runtime that drives
  suspended coroutines to completion.
- **`Spawn`** — fire-and-forget a background coroutine.

The goal is that asynchronous code reads like ordinary, straight-line code
— no callback pyramids, no manual state machines — while staying fully
testable (time, I/O and the scheduler are all injected through interfaces).

```cpp
Coro::Task<void> run(Coro::IScheduler& sched)
{
    auto user  = co_await fetch(sched, "/user/42");   // suspends, doesn't block
    auto posts = co_await fetch(sched, "/user/42/posts");
    std::cout << user << '\n' << posts << '\n';
}
```

---

## Why coroutines? (Best use cases)

A coroutine is a function that can **suspend** in the middle, hand control
back to whoever is driving it, and **resume** later exactly where it left
off — with all its local variables intact. That single capability is a
great fit for:

- **I/O-bound concurrency** — network requests, disk reads, timers,
  database calls. Thousands of in-flight operations can be multiplexed on a
  *single thread*, because a coroutine that is "waiting" costs only its
  small frame, not an OS thread with a stack.
- **Sequencing asynchronous steps** — "do A, then B with A's result, then
  C". With coroutines this is three lines of `co_await`; with callbacks
  it's three nested lambdas (the "callback hell" you've seen).
- **Structured concurrency** — run N things in parallel and wait for *all*
  of them (`WhenAll`) or the *first* one (`WhenAny`), then continue.
- **Cancellation & background work** — kick off a spinner / heartbeat /
  watchdog with `Spawn`, and stop it cooperatively via `std::stop_token`.
- **Lazy / generator-style pipelines** — because a `Task` does nothing
  until awaited, you can build a graph of work and only pay for what you
  consume.

**When *not* to reach for them:** CPU-bound number crunching that never
waits gains nothing from suspension — that wants real threads / SIMD /
parallel algorithms. Coroutines shine when the bottleneck is *waiting*, not
*computing*.

---

## The core concept

### `Task<T>` — a lazy, awaitable computation

Give a function the return type `Coro::Task<T>` and use `co_await` /
`co_return` inside it, and the compiler rewrites it into a coroutine:

```cpp
Coro::Task<int> answer()
{
    co_return 42;
}
```

Two things are important and often surprising:

1. **It's lazy.** Calling `answer()` does **not** run the body. It
   allocates a coroutine frame, leaves it *suspended at the start*, and
   returns a `Task<int>` that owns that frame. The body only runs once
   something *resumes* the task.
2. **It's move-only and owns its frame (RAII).** A `Task` destroys its
   coroutine frame when it goes out of scope. There is exactly one owner.

You consume a `Task` in one of two ways:

- **`co_await` it** from another coroutine — this resumes it and yields its
  result:
  ```cpp
  Coro::Task<void> caller()
  {
      int v = co_await answer();   // runs answer(), v == 42
  }
  ```
- **Hand it to a scheduler** to run it as a top-level / background task
  (see [`Spawn`](#spawn--fire-and-forget) and
  [`EventLoop::Run`](#running-it-the-scheduler)).

If the body throws, the exception is captured and **re-thrown at the
`co_await` site**, so ordinary `try`/`catch` works across suspension
points.

> **No stack overflow from deep chains.** When a task finishes it performs
> *symmetric transfer* — it tail-resumes directly into whoever awaited it
> instead of returning up the C++ call stack. A chain of a million
> `co_await`s runs in constant stack space. (See the heavily-commented
> [`src/Coro/Task.hpp`](src/Coro/Task.hpp) if you want the mechanics.)

---

## Awaitables — what you can `co_await`

An **awaitable** is *anything you are allowed to put after `co_await`*. It
is the extension point of the whole system: a `co_await` expression asks
the awaitable three questions, and the answers decide whether the current
coroutine suspends, who runs next, and what value the `co_await` produces.

The three questions form the **awaiter protocol**:

| Method | When the compiler calls it | What it does |
| --- | --- | --- |
| `bool await_ready()` | First. | Return `true` to skip suspending (result already available); `false` to suspend. |
| `… await_suspend(handle)` | After the coroutine has suspended. | You receive the *continuation* (the suspended coroutine). Arrange for it to be resumed later. Return type controls what runs next (see below). |
| `T await_resume()` | When the coroutine is resumed. | Produce the value of the whole `co_await` expression (or re-throw a stored exception). |

`await_suspend` can return three kinds of thing:

- **`void`** — "I've stashed the continuation somewhere; go back to the
  scheduler." (This is what `Sleep` does — it registers a timer.)
- **`bool`** — `false` means "actually, don't suspend, resume me now."
- **`std::coroutine_handle<>`** — "resume *this* coroutine next" (symmetric
  transfer). `Task`'s own awaiter returns the awaited task's handle, which
  is how awaiting a lazy task *starts* it.

`Coro` ships several awaitables built on this protocol.

### `Sleep` — suspend for a duration

```cpp
using namespace std::chrono_literals;
co_await Coro::Sleep(scheduler, 100ms);   // resumes ~100ms later
```

`await_suspend` registers the continuation with the scheduler to be resumed
at `now + duration`, then returns control to the loop. Nothing blocks; the
thread is free to run other coroutines in the meantime.

### `WhenAll` — run several tasks concurrently, wait for all

```cpp
auto [user, posts, stats] = co_await Coro::WhenAll(scheduler,
        fetch(scheduler, "/user/42",       250ms),
        fetch(scheduler, "/user/42/posts", 250ms),
        fetch(scheduler, "/user/42/stats", 250ms));
// total elapsed ≈ 250ms (the slowest), not 750ms (the sum)
```

It returns a `std::tuple` of each child's result. The children run
*concurrently* on the scheduler; `WhenAll` resumes the parent only after
the last one finishes. (`Task<void>` children contribute no tuple element.)

### `WhenAny` — race tasks, take the first to finish

```cpp
std::vector<Coro::Task<std::string>> racers;
racers.push_back(fetch(scheduler, "/mirror-a", 300ms));
racers.push_back(fetch(scheduler, "/mirror-b", 120ms));

Coro::WhenAnyResult<std::string> first =
        co_await Coro::WhenAny(scheduler, std::move(racers));

std::cout << "winner #" << first.index << ": " << first.value << '\n';
```

`WhenAnyResult<T>` carries the winning child's `index` and its `value`. The
first completion wins; later completions are dropped. Combined with
`std::stop_token` this gives you timeouts and "fastest mirror" patterns.

### `Spawn` / `scheduler.Post` — fire-and-forget

```cpp
scheduler.Post(heartbeat(scheduler, token));   // detached background task
co_await realWork(scheduler);                  // foreground continues
```

Posting a `Task<void>` to the scheduler detaches it: the scheduler drives
it to completion alongside everything else. Use it for background work
whose result you don't await inline — spinners, heartbeats, watchdogs.

`scheduler.Post(task)` is the member form; the free function
`Coro::Spawn(scheduler, task)` is a thin alias that forwards to it, so the
two are interchangeable:

```cpp
Coro::Spawn(scheduler, heartbeat(scheduler, token));   // same thing
```

Both take `Task<void>` specifically: a bare `Task` is lazy and would
otherwise never start, and a *value-returning* task can't be detached
because its result would be silently dropped — the type system forbids it.
To run a `Task<T>` and keep its value, `co_await` it (see
[below](#scheduling-a-value-returning-taskt)).

---

### Scheduling a value-returning `Task<T>`

`scheduler.Post` / `Spawn` only accept `Task<void>`, because a detached
task has no caller to hand a result to — posting a `Task<int>` would throw
the `int` away. To run a value-returning task you **await** it, which is
how you get its result back:

```cpp
Coro::Task<void> run(Coro::IScheduler& sched)
{
    int n = co_await compute(sched);   // runs compute(), n is its result
}
```

If you want a `Task<T>` to run *concurrently* with other work and still
collect its value, await it via a combinator instead of detaching it:

```cpp
// Run two value-returning tasks at once, keep both results:
auto [a, b] = co_await Coro::WhenAll(sched, computeA(sched), computeB(sched));

// Or wrap-and-detach: a void task that stores the result somewhere you own.
Coro::Task<void> store(Coro::IScheduler& sched, int& out)
{
    out = co_await compute(sched);
    co_return;
}
sched.Post(store(sched, myResult));   // now it's a Task<void> — fine to detach
```

The rule of thumb: **`Post`/`Spawn` for side effects, `co_await` (directly
or through `WhenAll`/`WhenAny`) for results.**

## Writing your own awaitable

Because the awaiter protocol is open, you can make *anything* awaitable —
an OS event, a condition variable, a one-shot signal. Here is a minimal
awaitable that resumes the coroutine on the *next* scheduler iteration
(a cooperative "yield"):

```cpp
#include <Coro/Scheduler.hpp>
#include <coroutine>

/// Awaitable that yields control back to the scheduler once, then resumes.
class YieldToScheduler
{
  public:
    explicit YieldToScheduler(Coro::IScheduler& scheduler) noexcept
        : _scheduler{scheduler} {}

    // Always suspend — the whole point is to give other tasks a turn.
    bool await_ready() const noexcept { return false; }

    // Re-post the continuation so the loop resumes it on its next pass.
    void await_suspend(std::coroutine_handle<> continuation) noexcept
    {
        _scheduler.Post(continuation);
    }

    // co_await produces no value.
    void await_resume() const noexcept {}

  private:
    Coro::IScheduler& _scheduler;
};

// Usage:
Coro::Task<void> chatty(Coro::IScheduler& sched)
{
    for (int i = 0; i < 3; ++i)
    {
        std::cout << "tick " << i << '\n';
        co_await YieldToScheduler{sched};   // let other tasks run between ticks
    }
}
```

The recipe is always the same:

1. **`await_ready`** — return `true` only if the result is already
   available and you can skip suspension entirely.
2. **`await_suspend(continuation)`** — you now own the suspended
   coroutine. Stash it somewhere (a timer queue, an OS callback, a
   `WhenAll` slot) and arrange for it to be `Post`ed / `resume()`d when
   your event fires. Return `void` to go back to the loop, or a
   `coroutine_handle<>` to transfer directly into another coroutine.
3. **`await_resume`** — return the value the `co_await` expression should
   produce (or re-throw a captured exception).

> **Design note (dependency injection).** Notice the awaitable takes an
> `IScheduler&`, not a global "current loop". Everything ambient — time,
> the scheduler, I/O — is reached through an injected interface. That seam
> is what lets the unit tests drive a deterministic fake clock instead of
> real wall-clock time. See [`AGENT.md`](AGENT.md) for the project's
> design rules.

---

## Running it — the scheduler

Coroutines are inert until something resumes them. `Coro` provides a
single-threaded, cooperative `EventLoop` (an `IScheduler`) that does the
driving:

```cpp
#include <Coro/Clock.hpp>
#include <Coro/EventLoop.hpp>
#include <Coro/Task.hpp>

int main()
{
    auto clock = Coro::SystemClock{};        // real time source (injected)
    auto loop  = Coro::EventLoop{clock};

    loop.Run(run(loop));   // drive the root task to completion
    return 0;
}
```

`EventLoop::Run(Task<void> root)` posts the root coroutine and then
iterates: drain the ready queue of resumable handles, fire any expired
timers, sleep until the next timer, repeat — until the root task is done or
the loop runs dry. The time source is the injected `IClock`
(`SystemClock` in production; a `ManualClock` in tests advances time by
hand for deterministic, instant tests).

Key types at a glance:

| Type | Role |
| --- | --- |
| `Coro::Task<T>` | Lazy, awaitable coroutine returning `T`. |
| `Coro::IScheduler` | Interface awaitables talk to (`Post`, `Post(Task<void>&&)`, `ScheduleAt`, `Clock`). |
| `Coro::EventLoop` | Concrete single-threaded cooperative scheduler. |
| `Coro::IClock` / `SystemClock` | Injected time source. |
| `Coro::Sleep` | Awaitable: suspend for a duration. |
| `Coro::WhenAll` | Awaitable: run tasks concurrently, collect all results. |
| `Coro::WhenAny` | Awaitable: race tasks, take the first. |
| `Coro::Spawn` | Detach a `Task<void>` onto a scheduler. |

---

## Building

Requires a C++23 compiler with coroutine support (recent Clang or GCC) and
CMake ≥ 3.28. The project ships CMake presets:

```sh
# Configure + build (debug, with clang-tidy enabled)
cmake --preset clang-debug
cmake --build --preset clang-debug

# Run the tests
ctest --preset clang-debug

# Other presets: clang-release, gcc-debug, gcc-release, clang-asan-ubsan
```

Build outputs land under `out/build/<preset>/`; demo executables under the
runtime output dir (`target/`).

### Demos

A set of single-file demos under [`src/demos`](src/demos) each illustrate one
idea (they're sized to fit on a lightning-talk slide):

| Demo | Shows |
| --- | --- |
| `demo_typewriter` | Driving output with `Sleep`. |
| `demo_progress` | A timed progress bar. |
| `demo_sequential_fetch` | `co_await` steps in sequence (the "after callbacks" picture). |
| `demo_when_all` | Three fetches in parallel with `WhenAll` (elapsed = max, not sum). |
| `demo_spinner_cancel` | `Spawn` a background spinner, cancel via `std::stop_token`. |
| `demo_dashboard` | Several animated regions multiplexed on one thread. |

---

## Library layout

```
src/Coro/
  Task.hpp        — Task<T>: the coroutine return type (start here)
  Scheduler.hpp   — IScheduler interface
  EventLoop.hpp   — concrete single-threaded scheduler
  Clock.hpp       — IClock / SystemClock time seam
  Sleep.hpp       — Sleep awaitable
  WhenAll.hpp     — run-all-and-wait awaitable
  WhenAny.hpp     — race-and-take-first awaitable
  Spawn.hpp       — detach a Task<void> onto a scheduler
```

Each header is self-contained and the public surface lives in the
`Coro` namespace. [`src/Coro/Task.hpp`](src/Coro/Task.hpp) is extensively
documented and is the best place to learn how the C++23 coroutine
machinery (promise types, awaiters, symmetric transfer) actually fits
together.

---

## License

Apache-2.0 — see SPDX headers in each source file.
