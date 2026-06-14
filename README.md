# Coro — an easy-to-use C++23 coroutines library

`Coro` is a small, dependency-free library that turns the raw C++23
coroutine *language constructs* (`co_await`, `co_return`, promise types,
coroutine handles) into a handful of building blocks you can actually use:

- **`Task<T>`** — the return type that makes any function a coroutine.
- **Awaitables** — the things you `co_await` (`Sleep`, `WhenAll`, `WhenAny`,
  or your own).
- **`IScheduler` / `EventLoop`** — the cooperative runtime that drives
  suspended coroutines to completion, with drop-in alternatives: a
  virtual-time scheduler for tests, a tracing decorator, and a Win32
  message-pump adapter.
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
  (see [`Spawn`](#spawn--schedulerpost--fire-and-forget) and
  [`EventLoop::Run`](#running-it--the-scheduler)).

If the body throws, the exception is captured and **re-thrown at the
`co_await` site**, so ordinary `try`/`catch` works across suspension
points.

> **No stack overflow from deep chains.** When a task finishes it performs
> *symmetric transfer* — it tail-resumes directly into whoever awaited it
> instead of returning up the C++ call stack. A chain of a million
> `co_await`s runs in constant stack space. (The next section explains
> the mechanism.)

---

## How C++23 coroutines actually work

You can use `Coro` without reading this section — but coroutines stop
being mysterious (and become much harder to misuse) once you know what
the compiler actually does with them. C++ ships only the *mechanics* of
coroutines: three keywords and a set of customization points. There is
**no** task type, no event loop, no `sleep` in the standard library —
libraries like this one supply those. Everything below is illustrated
with `Coro::Task`, and every claim can be checked against the
heavily-commented [`src/Coro/Task.hpp`](src/Coro/Task.hpp).

### A coroutine is a rewritten function

A function *is* a coroutine if its body contains at least one `co_await`,
`co_return`, or `co_yield`. Nothing in the signature says so — instead,
the **return type** decides how the coroutine behaves, by exposing a
nested `promise_type` (for `Task<T>` that is `Detail::TaskPromise<T>`).
The compiler rewrites the body into a resumable state machine around
that promise. Conceptually:

```cpp
Coro::Task<int> answer()
{
    co_return 42;
}

// ...is rewritten by the compiler into (simplified pseudo-code):
Coro::Task<int> answer()
{
    // 1. Allocate the coroutine frame. Parameters, locals, the promise
    //    object and the "where to resume" bookmark all live inside it.
    auto* frame = new __answer_frame {};

    // 2. Ask the promise for the object the caller receives.
    Coro::Task<int> task = frame->promise.get_return_object();

    // 3. initial_suspend(): Task's promise returns std::suspend_always,
    //    so the coroutine suspends *before its first statement* and the
    //    caller gets the still-lazy `task` back.
    co_await frame->promise.initial_suspend();

    // ---- everything below runs only once somebody resumes the task ----

    try
    {
        frame->promise.return_value(42);            // co_return 42;
    }
    catch (...)
    {
        frame->promise.unhandled_exception();       // capture, don't crash
    }

    // 4. final_suspend(): suspend one last time (so the stored result
    //    outlives the body) and hand control to whoever awaited us.
    co_await frame->promise.final_suspend();
}
```

### The coroutine frame and `std::coroutine_handle`

The **frame** is the coroutine's activation record, allocated when the
coroutine is first called. It holds the parameters (copied/moved in),
every local variable that lives across a suspension point, the promise
object, and a bookmark recording where to resume. *Suspending* means
"write the bookmark, return to whoever resumed us"; *resuming* means
"jump back to the bookmark". No threads, no signals, no magic — a
suspended coroutine is just a heap object waiting for someone to call
`resume()` on it.

`std::coroutine_handle<>` is a type-erased, **non-owning** pointer to a
frame with three operations:

```cpp
handle.resume();    // continue executing at the bookmark
handle.done();      // is the body finished (suspended at final_suspend)?
handle.destroy();   // free the frame
```

It behaves like a raw pointer: trivially copyable, no lifetime tracking.
Every use-after-free and double-destroy hazard of coroutines lives in
this type — which is why `Coro` wraps handles in single-owner RAII types
(`Task`, its awaiter) and why schedulers only ever *borrow* handles. The
typed variant `std::coroutine_handle<Promise>` additionally converts
between handle and promise (`.promise()`,
`std::coroutine_handle<Promise>::from_promise(p)`), which is how a
finished task finds its continuation.

### The promise — the coroutine's control block

The promise is the customization hub: the compiler calls a fixed set of
hooks on it at well-defined points, and the answers define the coroutine
type's entire personality. `Task`'s promise
([`Detail::TaskPromise<T>`](src/Coro/Task.hpp)) answers like this:

| Hook | Compiler calls it | What `Task`'s promise does |
| --- | --- | --- |
| `get_return_object()` | Once, at the initial call. | Builds the `Task<T>` that owns the frame. |
| `initial_suspend()` | Before the body's first statement. | Returns `std::suspend_always` — **this one line is why `Task` is lazy.** |
| `return_value(v)` / `return_void()` | At `co_return`. | Stores the result inside the promise. |
| `unhandled_exception()` | When an exception escapes the body. | Captures `std::current_exception()` for re-throw at the `co_await` site. |
| `final_suspend()` | After the body finishes. | Returns a `FinalAwaiter` that transfers control to the continuation (see below). |

An *eager* task library would return `std::suspend_never` from
`initial_suspend()` and the body would start running immediately at the
call. `Task` deliberately chooses lazy: a task can be created, moved,
stored, and composed (`WhenAll(a(), b())`) before any of its code runs,
and a task that is never awaited simply frees its frame without side
effects.

### What a `co_await` expression compiles into

`co_await` is a negotiation between the suspending coroutine and the
*awaiter* object. The expression `auto v = co_await expr;` expands
roughly to:

```cpp
auto&& awaiter = /* expr, or expr.operator co_await() if it has one */;

if (!awaiter.await_ready())                  // 1. result already there? then don't suspend
{
    /* suspend: write the resume bookmark into the frame */
    awaiter.await_suspend(thisHandle);       // 2. hand over the continuation
    /* control leaves this coroutine — back to the resumer/scheduler */
    /* ...time passes... someone calls thisHandle.resume() */
}

auto v = awaiter.await_resume();             // 3. value of the co_await expression
```

Two details matter in practice:

- The coroutine is *already suspended* when `await_suspend` runs. That is
  what makes it safe for the awaiter to hand the continuation to a timer
  queue, another thread, or an OS callback that might resume it
  immediately.
- `await_suspend`'s return type steers what runs next: `void` (return to
  the resumer), `bool` (`false` = cancel the suspension), or a
  `std::coroutine_handle<>` (resume *that* coroutine right now). The
  three forms are tabulated in
  [Awaitables](#awaitables--what-you-can-co_await) below.

### Symmetric transfer — chaining without stack growth

When `await_suspend` returns a `std::coroutine_handle<>`, the runtime
resumes that handle **as a tail call**: the current resume call returns
and the new coroutine starts in its place, without growing the C++ call
stack. `Task` uses this trick in both directions:

```text
parent: co_await child()                 child's body finishes
   │ parent suspends                        │ child suspends at final_suspend()
   ▼                                        ▼
TaskAwaiter::await_suspend           FinalAwaiter::await_suspend
   records parent as the                reads promise.continuation
   child's continuation,
   returns child's handle ──────▶       returns parent's handle ──────▶ parent resumes;
      (tail-resume: this is what           (tail-resume: result            await_resume()
       *starts* the lazy child)             delivery, no stack)            yields the value
```

Every `──▶` is a tail-resume, not a nested function call — so an await
chain a million tasks deep still runs in constant stack space. Without
symmetric transfer (i.e. with `await_suspend` returning `void` and
someone calling `resume()` recursively), each link would add a stack
frame and deep chains would overflow.

### Who destroys the frame?

`coroutine_handle::destroy()` must be called exactly once per frame.
`Coro` makes that a single-owner discipline with three states:

| State of the task | Owner of the frame | Frame is destroyed by |
| --- | --- | --- |
| Created, not yet consumed | The `Task<T>` object. | `Task`'s destructor (or move-assignment over it). |
| Being `co_await`-ed | The `TaskAwaiter` — the rvalue-qualified `operator co_await()` *steals* the handle, leaving the `Task` empty. | The awaiter, at the end of the `co_await` full-expression. |
| Detached (`Spawn` / `scheduler.Post(std::move(task))`) | The frame itself (`promise.detached == true`). | `FinalAwaiter`, immediately after the body finishes. |

The hand-offs exist for a reason: stealing the handle into the awaiter
means a temporary `Task` destroyed mid-`co_await` cannot tear the frame
down underneath the suspension, and detached frames self-destroying at
`final_suspend` means schedulers never have to track ownership at all —
they only ever borrow handles.

That is the entire language feature: a rewritten function body, a frame,
a handle, a promise with hooks, and the awaiter protocol. Everything
else — tasks, sleeping, racing, event loops — is library code, and
[`src/Coro/Task.hpp`](src/Coro/Task.hpp) walks through all of it with a
comment on every step.

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

`EventLoop` is only the default `IScheduler`. The library ships three
more — `ManualScheduler` (virtual time for tests), `TracingScheduler` (a
decorator that records every scheduling call), and
`Win32MessageScheduler` (rides a GUI message pump) — covered in
[Extending Coro](#extending-coro) and
[Testing coroutine code](#testing-coroutine-code).

Key types at a glance:

| Type | Role |
| --- | --- |
| `Coro::Task<T>` | Lazy, awaitable coroutine returning `T`. |
| `Coro::IScheduler` | Interface awaitables talk to (`Post`, `Post(Task<void>&&)`, `ScheduleAt`, `Clock`). |
| `Coro::EventLoop` | Concrete single-threaded cooperative scheduler. |
| `Coro::ManualScheduler` | Virtual-time scheduler for deterministic tests (`RunUntilIdle`, `AdvanceBy`, `AdvanceTo`). |
| `Coro::TracingScheduler` | Decorator that records every `Post`/`ScheduleAt` on the wrapped scheduler. |
| `Coro::Win32MessageScheduler` | Scheduler riding an existing Win32 GUI message pump (Windows only). |
| `Coro::TimerQueue` | Min-heap of (deadline, handle) entries — the timer engine the schedulers compose. |
| `Coro::IClock` / `SystemClock` | Injected time source. |
| `Coro::Sleep` | Awaitable: suspend for a duration. |
| `Coro::WhenAll` | Awaitable: run tasks concurrently, collect all results. |
| `Coro::WhenAny` | Awaitable: race tasks, take the first. |
| `Coro::Spawn` | Detach a `Task<void>` onto a scheduler. |

---

## Extending Coro

The library has exactly two extension seams, and they compose freely:
**awaitables** (new things to put after `co_await`) and **schedulers**
(new runtimes to drive coroutines). Both are deliberately small.

### Writing your own awaitable

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

### Writing your own scheduler

Every runtime in `Coro` — including the test and GUI ones — is an
implementation of the three-method `IScheduler` interface
([`src/Coro/Scheduler.hpp`](src/Coro/Scheduler.hpp)):

```cpp
class IScheduler
{
  public:
    /// Post a ready continuation to the run queue; resume it on the
    /// next iteration.
    virtual void Post(std::coroutine_handle<> handle) = 0;

    /// Resume `handle` at (or shortly after) `when` on this
    /// scheduler's clock.
    virtual void ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle) = 0;

    /// The clock awaitables read `Now()` from to compute deadlines.
    [[nodiscard]] virtual IClock const& Clock() const noexcept = 0;
};
```

The contract you must honour:

- **Handles are borrowed.** Never `destroy()` a posted handle — ownership
  stays with the `Task`/awaiter that scheduled it (detached frames own
  themselves). Dropping un-run handles on shutdown is fine; destroying
  them is not.
- **Single-threaded by default.** Callers run on the loop thread.
  (`Win32MessageScheduler::Post` is the documented exception — it is
  safe from any thread because `PostMessage` is.)
- **The `Task<void>` overload comes free.** `IScheduler` itself provides
  the non-virtual `Post(Task<void>&&)` convenience (detach + post); add
  `using IScheduler::Post;` next to your override so it stays visible.

A minimal scheduler is mostly two containers — and the timer half is
already written for you: `Coro::TimerQueue`
([`src/Coro/TimerQueue.hpp`](src/Coro/TimerQueue.hpp)) is the min-heap of
(deadline, handle) entries with FIFO tiebreak that all in-tree schedulers
compose (`Schedule(when, handle)`, `PopDue(now)`, `NextDeadline()`):

```cpp
#include <Coro/Scheduler.hpp>
#include <Coro/TimerQueue.hpp>

#include <coroutine>
#include <deque>

class MyScheduler final: public Coro::IScheduler
{
  public:
    explicit MyScheduler(Coro::IClock& clock) noexcept: _clock { clock } {}

    using IScheduler::Post; // keep the Task<void>&& convenience overload visible

    void Post(std::coroutine_handle<> handle) override { _ready.push_back(handle); }

    void ScheduleAt(Coro::IClock::TimePoint when, std::coroutine_handle<> handle) override
    {
        _timers.Schedule(when, handle);
    }

    [[nodiscard]] Coro::IClock const& Clock() const noexcept override { return _clock; }

    // Your driving logic goes here: pop _ready front-to-back and resume(),
    // move due timers (`_timers.PopDue(_clock.Now())`) into _ready, and
    // decide how to wait until `_timers.NextDeadline()`.

  private:
    Coro::IClock& _clock;
    std::deque<std::coroutine_handle<>> _ready;
    Coro::TimerQueue _timers;
};
```

The three non-default schedulers in the tree are worked examples of this
recipe, each teaching a different lesson:

**`ManualScheduler` — own the clock.**
([`src/Coro/ManualScheduler.hpp`](src/Coro/ManualScheduler.hpp)) owns a
*virtual* clock that starts at the epoch and only moves when told to, and
exposes an Rx-`TestScheduler`-style driving API: `RunUntilIdle()` drains
ready work without moving time; `AdvanceBy(delta)` / `AdvanceTo(target)`
move virtual time and fire every timer on the way (each timer observes
`Clock().Now()` equal to its own deadline). Introspection
(`ReadyCount()`, `PendingTimerCount()`, `NextDeadline()`) lets tests
assert on scheduling behaviour directly.

```cpp
auto scheduler = Coro::ManualScheduler {};
scheduler.Post(backgroundWork(scheduler));   // detach a Task<void>
scheduler.AdvanceBy(50ms);                   // fire timers, resume work — instantly
```

**`TracingScheduler` — you don't have to be a runtime at all.**
([`src/Coro/TracingScheduler.hpp`](src/Coro/TracingScheduler.hpp)) is a
*decorator*: it wraps any inner `IScheduler`, records every `Post` /
`ScheduleAt` as a `TraceEvent`, and forwards the call unchanged. Tests
assert on the recording (`Events()`, `Count(kind)`); demos attach a
`Sink` callback to print the machinery live.

```cpp
auto clock  = Coro::SystemClock {};
auto loop   = Coro::EventLoop { clock };
auto traced = Coro::TracingScheduler { loop };   // wraps any IScheduler

loop.Run(run(traced));                           // tasks talk to `traced`
// traced.Count(Coro::TraceEvent::Kind::ScheduleAt) == number of timers armed
```

**`Win32MessageScheduler` — bolt the contract onto a loop you don't own.**
([`src/Coro/Win32MessageScheduler.hpp`](src/Coro/Win32MessageScheduler.hpp),
Windows only) maps `IScheduler` onto an *existing* GUI message pump:
`Post` becomes a `WM_APP`-range message dispatched by the application's
own `GetMessage` loop, and `ScheduleAt` parks handles in a `TimerQueue`
behind a single `SetTimer` slot armed for the earliest deadline. A button
handler can `Spawn` a task that `co_await`s `Sleep`/`WhenAll` chains
while the UI stays responsive — no `WM_TIMER` state machines. Note the
project-style fallible constructor:

```cpp
auto clock     = Coro::SystemClock {};
auto scheduler = Coro::Win32MessageScheduler::Create(clock);  // std::expected
if (!scheduler)
    return report(scheduler.error());          // std::error_code from GetLastError

Coro::Spawn(**scheduler, fadeInStatus(**scheduler));
// resumes ride the message pump; the GUI thread never blocks
```

If your environment has its own loop — Qt, glib, an audio callback, a
game engine tick — the same pattern applies: translate `Post` into "run
this on the loop", keep deadlines in a `TimerQueue`, and arm whatever
native timer the platform offers for `NextDeadline()`.

---

## Testing coroutine code

Tests use Catch2 and live next to the implementation — `Foo.hpp` is
covered by `Foo_test.cpp` in the same directory — and run via
`ctest --preset clang-debug`. Because time and scheduling are injected
interfaces, coroutine tests are deterministic and instant: nothing ever
sleeps on the wall clock.

The workhorse is `ManualScheduler`. A real test from
[`src/Coro/Sleep_test.cpp`](src/Coro/Sleep_test.cpp):

```cpp
TEST_CASE("Two sequential Sleeps add their deadlines", "[Sleep]")
{
    auto scheduler = Coro::ManualScheduler {};
    auto reached = 0;

    auto const root = [&]() -> Coro::Task<void> {
        co_await Coro::Sleep(scheduler, 30ms);
        ++reached;
        co_await Coro::Sleep(scheduler, 30ms);
        ++reached;
    }();

    scheduler.Post(root.Native());
    scheduler.RunUntilIdle(); // suspends at the first Sleep
    REQUIRE(reached == 0);

    scheduler.AdvanceBy(30ms); // first Sleep fires, suspends at the second
    REQUIRE(reached == 1);

    scheduler.AdvanceBy(30ms);
    REQUIRE(reached == 2);
    REQUIRE(root.IsReady());
}
```

The pattern: create the lazy root task, post its handle
(`scheduler.Post(root.Native())` keeps ownership in the test so
`root.IsReady()` stays inspectable), drain with `RunUntilIdle()`, then
step virtual time with `AdvanceBy`/`AdvanceTo` and assert after each
step. `PendingTimerCount()` / `NextDeadline()` let you assert on the
*scheduling* itself, not just on task side effects.

Two more test seams when you need them:

- **`EventLoop` + `ManualClock`** ([`src/tests/ManualClock.hpp`](src/tests/ManualClock.hpp))
  drives the *production* loop under fake time: interleave
  `clock.Advance(delta)` with `loop.RunOnce()` to single-step, or call
  `loop.Run(...)` outright — `ManualClock::WaitUntil` jumps to the next
  deadline instead of sleeping, so even full runs finish instantly.
- **`TracingScheduler`** wraps either of the above when the assertion is
  about scheduler interactions ("exactly one timer was armed, at
  t+50ms") rather than results.

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

### macOS

The `clang-debug` / `clang-release` presets work on macOS with the same
names. They deliberately use **Homebrew's LLVM clang**, not the Apple Clang
that ships as `/usr/bin/clang` — Apple Clang lacks a matching `clang-tidy`
and the static UBSan runtime these presets rely on. Install it once with:

```sh
brew install llvm
```

A toolchain file ([`cmake/HomebrewLLVM.cmake`](cmake/HomebrewLLVM.cmake),
wired into the presets) locates the Homebrew LLVM keg automatically on both
Apple-silicon (`/opt/homebrew`) and Intel (`/usr/local`) Macs — it does not
need to be first on your `PATH`. If it lives somewhere unusual, point at it
with `-DHOMEBREW_LLVM_PREFIX=<path>`. On Linux the same presets fall back to
the plain `clang` / `clang++` on the `PATH`.

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
  Task.hpp                        — Task<T>: the coroutine return type (start here)
  Scheduler.hpp                   — IScheduler interface
  Clock.hpp                       — IClock / SystemClock time seam
  EventLoop.hpp / .cpp            — concrete single-threaded scheduler
  TimerQueue.hpp                  — min-heap of (deadline, handle); shared by all schedulers
  ManualScheduler.hpp / .cpp      — virtual-time scheduler for deterministic tests
  TracingScheduler.hpp            — decorator recording every Post/ScheduleAt
  Win32MessageScheduler.hpp / .cpp — rides a Win32 GUI message pump (Windows only)
  Sleep.hpp                       — Sleep awaitable
  WhenAll.hpp                     — run-all-and-wait awaitable
  WhenAny.hpp                     — race-and-take-first awaitable
  Spawn.hpp                       — detach a Task<void> onto a scheduler
```

Each header is self-contained and the public surface lives in the
`Coro` namespace. [`src/Coro/Task.hpp`](src/Coro/Task.hpp) is extensively
documented and is the best place to learn how the C++23 coroutine
machinery (promise types, awaiters, symmetric transfer) actually fits
together.

---

## License

Apache-2.0 — see SPDX headers in each source file.
