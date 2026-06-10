// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace Coro
{

/// Lazy coroutine return type representing a deferred computation that
/// yields a `T` (or nothing, for `Task<void>`).
///
/// @par What it is
/// `Task<T>` is the *return type* you give to any function that uses
/// `co_await` / `co_return` and should hand its result back to a caller.
/// Writing `Task<int> foo() { co_return 42; }` turns `foo` into a
/// coroutine: calling `foo()` does **not** run the body, it allocates a
/// coroutine frame, leaves it suspended, and returns a `Task<int>` that
/// owns that frame.
///
/// @par Lazy by construction
/// A Task starts suspended at `initial_suspend()` (which returns
/// `std::suspend_always`). Nothing in the body runs until someone
/// resumes it — either by `co_await`-ing the Task from another coroutine,
/// or by handing it to a scheduler (see @ref Spawn / `IScheduler::Post`).
/// This laziness is what lets you build a call graph of tasks up front
/// and only pay for them when awaited, and it is why a Task that is
/// created but never awaited/scheduled simply does nothing and cleanly
/// destroys its frame.
///
/// @par No stack growth (symmetric transfer)
/// When the body finishes it suspends at `final_suspend()` and performs a
/// *symmetric transfer* — it tail-resumes directly into whoever was
/// waiting on it (its "continuation") instead of returning up the C++
/// call stack. As a result a chain like
/// @code
/// Task<void> outer() {
///     co_await a();   // each co_await resumes the awaited task
///     co_await b();   // in place, then transfers back here
/// }
/// @endcode
/// runs in constant C++ stack space no matter how deep the await chain
/// goes — there is no risk of a stack overflow from deeply nested
/// `co_await`s. See @ref Detail::TaskPromiseBase::FinalAwaiter for the
/// mechanism.
///
/// @par Result and exception propagation
/// The value passed to `co_return` is stored in the promise and returned
/// from the `co_await` expression. If the body throws, the exception is
/// captured (`unhandled_exception`) and re-thrown at the point of
/// `co_await` — so `try { co_await t(); } catch (...)` works as you would
/// expect across the suspension boundary.
///
/// @par Ownership (move-only, RAII)
/// Tasks are move-only and own their coroutine frame: the destructor
/// destroys the frame if it still holds one. Awaiting a Task is the
/// canonical way to consume it — the `co_await` expression yields a
/// @ref Task::Awaiter that *takes ownership* of the coroutine handle for
/// the duration of the suspension and destroys it when the
/// full-expression ends. This is the RAII seam the project's AGENT.md
/// calls out: because `operator co_await` is rvalue-qualified and steals
/// the handle, a stray temporary `Task` cannot tear the coroutine down
/// across a suspend point.
///
/// @par Typical usage
/// @code
/// // Define a coroutine that returns a Task:
/// Coro::Task<int> answer() { co_return 42; }
///
/// // Consume it from another coroutine by awaiting:
/// Coro::Task<void> caller() {
///     int v = co_await answer();   // resumes answer(), yields 42
/// }
///
/// // Or fire-and-forget it onto a scheduler:
/// Coro::Spawn(scheduler, caller());
/// @endcode
///
/// @tparam T Result type produced by `co_return value;` in the body.
///           Use `Task<void>` for tasks that produce no value.
///
/// @see Spawn for handing a `Task<void>` to a scheduler.
/// @see Detail::TaskPromise for the promise that stores the result.
template <typename T = void>
class Task;

/// Implementation details of `Task`. The types here are the *promise* and
/// *awaiter* objects the C++ coroutine machinery requires; user code never
/// names them directly, but they are documented because they are where the
/// interesting coroutine mechanics live.
namespace Detail
{

    /// Promise base shared by `Task<T>` and `Task<void>`.
    ///
    /// Every coroutine has an associated *promise object* (its type is
    /// `Task<T>::promise_type`). The compiler creates one inside the
    /// coroutine frame and calls hook functions on it at well-defined
    /// points: `initial_suspend()` before the body runs, `final_suspend()`
    /// after it returns, `unhandled_exception()` if it throws, and
    /// `return_value()`/`return_void()` on `co_return`. This base provides
    /// the parts common to both the value and `void` flavours.
    ///
    /// It holds two pieces of state:
    /// - @ref continuation — who to resume when this task finishes, set by
    ///   the awaiter when the task is `co_await`-ed;
    /// - @ref exception — an exception escaping the body, stashed for
    ///   re-throw at the awaiting `co_await`.
    struct TaskPromiseBase
    {
        /// The coroutine to resume once this task completes (its caller /
        /// awaiter). Defaults to `std::noop_coroutine()` — a do-nothing
        /// handle — so that a task which finishes without ever having been
        /// awaited (e.g. a detached/spawned root) transfers into a safe
        /// no-op instead of an invalid handle.
        std::coroutine_handle<> continuation { std::noop_coroutine() };

        /// Exception captured by `unhandled_exception()` if the body threw.
        /// Null when the body completed normally. Re-thrown by the
        /// awaiter's `await_resume()`.
        std::exception_ptr exception {};

        /// Awaiter returned from `final_suspend()` that performs *symmetric
        /// transfer* to the continuation.
        ///
        /// The trick is in `await_suspend`: by returning another coroutine
        /// handle (the continuation), the C++ runtime tail-resumes directly
        /// into it instead of unwinding the C++ call stack. That is what
        /// keeps deep `co_await` chains running in constant stack space.
        struct FinalAwaiter
        {
            /// @return Always `false` — the task has finished, but we still
            ///         want `await_suspend` to run so it can transfer
            ///         control to the continuation.
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            /// Hands control to the finished task's continuation.
            /// @param self Handle to the just-finished coroutine, used to
            ///             reach its promise (and thus its continuation).
            /// @return The continuation handle; the runtime immediately
            ///         resumes it (symmetric transfer, no stack growth).
            template <typename Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept
            {
                return self.promise().continuation;
            }

            /// Never observed — the task is done; provided to satisfy the
            /// awaiter interface.
            void await_resume() const noexcept {}
        };

        /// Hook called before the coroutine body runs.
        /// @return `std::suspend_always` — this is what makes `Task` *lazy*:
        ///         the body does not start until the task is resumed by an
        ///         awaiter or a scheduler.
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept
        {
            return {};
        }

        /// Hook called after the body returns (or after `co_return`).
        /// @return A @ref FinalAwaiter that transfers control to the
        ///         continuation rather than returning up the stack.
        [[nodiscard]] FinalAwaiter final_suspend() const noexcept
        {
            return {};
        }

        /// Hook called if an exception escapes the coroutine body. Captures
        /// the in-flight exception so the awaiter can re-throw it at the
        /// `co_await` site instead of calling `std::terminate`.
        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }
    };

    /// Promise for a value-producing `Task<T>`.
    ///
    /// Adds storage for the result and the `return_value` hook the compiler
    /// calls on `co_return value;`. The result lives in a `std::variant`
    /// so that the "no value yet" state (`std::monostate`) is
    /// representable.
    ///
    /// @tparam T The value type produced by the coroutine.
    template <typename T>
    struct TaskPromise: TaskPromiseBase
    {
        /// Holds the produced value once the body completes.
        /// - index 0 (`std::monostate`): no value yet;
        /// - index 1 (`T`): the value from `co_return`;
        /// - index 2 (`std::exception_ptr`): reserved slot (exceptions are
        ///   actually carried in `TaskPromiseBase::exception`).
        std::variant<std::monostate, T, std::exception_ptr> result;

        /// Compiler hook that builds the owning `Task<T>` for this promise.
        /// Called once, when the coroutine is first invoked, to produce the
        /// object the caller receives.
        /// @return A `Task<T>` owning the handle to this coroutine frame.
        Task<T> get_return_object() noexcept;

        /// Compiler hook for `co_return value;`. Stores @p value as the
        /// task's result.
        /// @tparam U Deduced type of the returned expression; must be
        ///           convertible to `T`.
        /// @param value The value produced by the coroutine body.
        template <typename U>
            requires std::is_convertible_v<U&&, T>
        void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            result.template emplace<1>(std::forward<U>(value));
        }
    };

    /// Promise specialization for a `Task<void>` — a coroutine that
    /// produces no value. Identical to @ref TaskPromise "TaskPromise<T>"
    /// except it has no result storage and offers `return_void` (for
    /// `co_return;`) instead of `return_value`.
    template <>
    struct TaskPromise<void>: TaskPromiseBase
    {
        /// Compiler hook that builds the owning `Task<void>` for this
        /// promise.
        /// @return A `Task<void>` owning the handle to this coroutine frame.
        Task<void> get_return_object() noexcept;

        /// Compiler hook for `co_return;` (or falling off the end of a
        /// `void` coroutine). No value to store.
        void return_void() const noexcept {}
    };

    /// Common awaiter machinery shared by `Task<T>::Awaiter` and
    /// `Task<void>::Awaiter`.
    ///
    /// When you write `co_await someTask`, the compiler asks the resulting
    /// awaiter three questions in turn:
    /// 1. `await_ready()` — is the result already available? If so, skip
    ///    suspending.
    /// 2. `await_suspend(continuation)` — the awaiting coroutine is about
    ///    to suspend; do the wiring and decide who runs next.
    /// 3. `await_resume()` — produce the value of the `co_await` expression
    ///    when control comes back (implemented in the derived awaiters,
    ///    since it depends on whether `T` is `void`).
    ///
    /// This base implements (1) and (2): it records the awaiting coroutine
    /// as the awaited task's continuation, then transfers control *into*
    /// the awaited task — so awaiting a lazy task is what actually starts
    /// it running.
    ///
    /// @tparam T Result type of the awaited task.
    template <typename T>
    class TaskAwaiterBase
    {
      public:
        /// @param handle Handle to the awaited task's coroutine frame.
        explicit TaskAwaiterBase(std::coroutine_handle<TaskPromise<T>> handle) noexcept:
            _handle { handle }
        {
        }

        /// @return `true` if there is nothing to wait for — the handle is
        ///         empty or the coroutine has already finished — in which
        ///         case the awaiting coroutine does not suspend at all.
        [[nodiscard]] bool await_ready() const noexcept
        {
            return !_handle || _handle.done();
        }

        /// Wires the awaited task to resume @p continuation when it
        /// finishes, then transfers control into the awaited task.
        /// @param continuation The awaiting coroutine, suspended and
        ///                     waiting for the result.
        /// @return The awaited task's handle; returning it makes the
        ///         runtime resume the awaited task directly (symmetric
        ///         transfer) — this is what starts the lazy task running.
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            _handle.promise().continuation = continuation;
            return _handle;
        }

      protected:
        /// @return The handle to the awaited coroutine frame.
        [[nodiscard]] std::coroutine_handle<TaskPromise<T>> Coroutine() const noexcept
        {
            return _handle;
        }

        /// Re-point this awaiter at a different coroutine frame. Used by the
        /// derived awaiters' move constructors to keep the base handle in
        /// sync with the moved-in owned handle.
        /// @param handle The new coroutine handle.
        void SetCoroutine(std::coroutine_handle<TaskPromise<T>> handle) noexcept
        {
            _handle = handle;
        }

      private:
        std::coroutine_handle<TaskPromise<T>> _handle;
    };

} // namespace Detail

/// @copydoc Task
template <typename T>
class Task
{
  public:
    /// The promise type the compiler associates with this coroutine. Its
    /// presence as a nested `promise_type` is what makes `Task<T>` usable
    /// as a coroutine return type.
    using promise_type = Detail::TaskPromise<T>;

    /// Strongly-typed handle to this task's coroutine frame.
    using Handle = std::coroutine_handle<promise_type>;

    /// Constructs an empty Task that owns no coroutine frame. Awaiting or
    /// inspecting it behaves as an already-completed task (`IsReady()` is
    /// `true`).
    Task() noexcept = default;

    /// Wraps an existing coroutine handle, taking ownership of its frame.
    /// Normally only called by @ref promise_type::get_return_object; user
    /// code obtains a Task by calling a coroutine function, not via this
    /// constructor.
    /// @param handle Handle to the coroutine frame to own.
    explicit Task(Handle handle) noexcept:
        _handle { handle }
    {
    }

    /// Tasks are non-copyable — a coroutine frame has a single owner.
    Task(Task const&) = delete;
    /// Tasks are non-copyable — a coroutine frame has a single owner.
    Task& operator=(Task const&) = delete;

    /// Move-constructs, transferring frame ownership and leaving @p other
    /// empty.
    /// @param other The task to move from; left owning no frame.
    Task(Task&& other) noexcept:
        _handle { std::exchange(other._handle, {}) }
    {
    }

    /// Move-assigns, destroying any frame currently owned, then taking
    /// @p other's frame and leaving it empty.
    /// @param other The task to move from; left owning no frame.
    /// @return `*this`.
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    /// Destroys the owned coroutine frame, if any. Destroying a Task that
    /// was never started simply frees the frame; the body never runs.
    ~Task()
    {
        if (_handle)
            _handle.destroy();
    }

    /// @return `true` if this task has run to completion, or is in the
    ///         empty / moved-from state; `false` if it has a frame that
    ///         has not finished yet.
    [[nodiscard]] bool IsReady() const noexcept
    {
        return !_handle || _handle.done();
    }

    /// Raw, non-owning access to the underlying coroutine handle.
    ///
    /// Used to drive the task outside of `co_await` — e.g. a scheduler that
    /// wants to `resume()` it, or a test that pumps it manually. Ownership
    /// stays with this Task, so the handle is only valid while this Task is
    /// alive; use @ref Release if you intend to transfer ownership.
    /// @return The coroutine handle (possibly null).
    [[nodiscard]] Handle Native() const noexcept
    {
        return _handle;
    }

    /// Relinquishes ownership of the coroutine frame to the caller.
    ///
    /// After this call the Task is empty and its destructor will not touch
    /// the frame — the caller is now responsible for resuming it to
    /// completion and/or destroying it. This is how @ref Spawn hands a
    /// task to a scheduler: the scheduler owns the frame and destroys it
    /// when the task finishes.
    /// @return The released handle (possibly null).
    [[nodiscard]] Handle Release() noexcept
    {
        return std::exchange(_handle, {});
    }

    /// Awaiter produced by `co_await task`.
    ///
    /// It takes ownership of the coroutine handle for the duration of the
    /// suspension; the rvalue `Task` that produced it is left empty (see
    /// @ref Task::operator co_await) so its destructor cannot tear the
    /// coroutine down underneath us mid-await. The awaiter destroys the
    /// handle when the full-expression ends. Move-only and non-assignable
    /// to keep that single-owner invariant.
    ///
    /// @see Detail::TaskAwaiterBase for the `await_ready`/`await_suspend`
    ///      half of the protocol.
    class Awaiter: public Detail::TaskAwaiterBase<T>
    {
      public:
        /// @param handle Handle to the awaited task's frame; ownership is
        ///               taken for the duration of the await.
        explicit Awaiter(Handle handle) noexcept:
            Detail::TaskAwaiterBase<T> { handle }
        {
        }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;

        /// Move-constructs, transferring frame ownership and keeping the
        /// base class's handle in sync.
        /// @param other The awaiter to move from; left owning no frame.
        Awaiter(Awaiter&& other) noexcept:
            Detail::TaskAwaiterBase<T> { std::exchange(other._owned, Handle {}) }
        {
            this->SetCoroutine(_owned);
        }
        Awaiter& operator=(Awaiter&&) = delete;

        /// Destroys the owned coroutine frame at the end of the
        /// full-expression containing the `co_await`.
        ~Awaiter()
        {
            if (_owned)
                _owned.destroy();
        }

        /// Produces the value of the `co_await` expression once the awaited
        /// task has finished.
        /// @return The value the awaited task produced via `co_return`.
        /// @throws Re-throws any exception that escaped the awaited task's
        ///         body, so it surfaces at the `co_await` site.
        T await_resume()
        {
            auto& promise = this->Coroutine().promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
            return std::move(std::get<1>(promise.result));
        }

      private:
        /// The frame this awaiter owns and will destroy.
        Handle _owned { this->Coroutine() };
    };

    /// Makes a Task awaitable. Rvalue-qualified so you can only await a
    /// Task you own outright (a temporary or an explicitly `std::move`-d
    /// one) — this is what prevents two awaiters from racing over one
    /// frame. The handle is stolen into the returned @ref Awaiter, leaving
    /// this Task empty.
    /// @return An @ref Awaiter owning this task's frame.
    Awaiter operator co_await() && noexcept
    {
        return Awaiter { std::exchange(_handle, {}) };
    }

  private:
    /// The owned coroutine frame, or null when empty / moved-from.
    Handle _handle {};
};

/// Specialization of @ref Task for coroutines that produce no value.
///
/// Behaves exactly like `Task<T>` — same laziness, ownership, symmetric
/// transfer and exception propagation — except the body ends with
/// `co_return;` (or falls off the end) and `co_await`-ing it yields no
/// value. This is the common type for fire-and-forget work spawned onto a
/// scheduler (see @ref Spawn).
template <>
class Task<void>
{
  public:
    /// @copydoc Task::promise_type
    using promise_type = Detail::TaskPromise<void>;

    /// @copydoc Task::Handle
    using Handle = std::coroutine_handle<promise_type>;

    /// @copydoc Task::Task()
    Task() noexcept = default;

    /// @copydoc Task::Task(Handle)
    explicit Task(Handle handle) noexcept:
        _handle { handle }
    {
    }

    /// Tasks are non-copyable — a coroutine frame has a single owner.
    Task(Task const&) = delete;
    /// Tasks are non-copyable — a coroutine frame has a single owner.
    Task& operator=(Task const&) = delete;

    /// @copydoc Task::Task(Task&&)
    Task(Task&& other) noexcept:
        _handle { std::exchange(other._handle, {}) }
    {
    }

    /// @copydoc Task::operator=(Task&&)
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    /// @copydoc Task::~Task
    ~Task()
    {
        if (_handle)
            _handle.destroy();
    }

    /// @copydoc Task::IsReady
    [[nodiscard]] bool IsReady() const noexcept
    {
        return !_handle || _handle.done();
    }

    /// @copydoc Task::Native
    [[nodiscard]] Handle Native() const noexcept
    {
        return _handle;
    }

    /// @copydoc Task::Release
    [[nodiscard]] Handle Release() noexcept
    {
        return std::exchange(_handle, {});
    }

    /// Awaiter produced by `co_await task` for a `Task<void>`. Identical to
    /// @ref Task::Awaiter except `await_resume` yields nothing.
    class Awaiter: public Detail::TaskAwaiterBase<void>
    {
      public:
        /// @copydoc Task::Awaiter::Awaiter(Handle)
        explicit Awaiter(Handle handle) noexcept:
            Detail::TaskAwaiterBase<void> { handle }
        {
        }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;

        /// @copydoc Task::Awaiter::Awaiter(Awaiter&&)
        Awaiter(Awaiter&& other) noexcept:
            Detail::TaskAwaiterBase<void> { std::exchange(other._owned, Handle {}) }
        {
            this->SetCoroutine(_owned);
        }
        Awaiter& operator=(Awaiter&&) = delete;

        /// @copydoc Task::Awaiter::~Awaiter
        ~Awaiter()
        {
            if (_owned)
                _owned.destroy();
        }

        /// Completes the `co_await` expression. Produces no value.
        /// @throws Re-throws any exception that escaped the awaited task's
        ///         body, so it surfaces at the `co_await` site.
        void await_resume()
        {
            auto& promise = this->Coroutine().promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
        }

      private:
        /// The frame this awaiter owns and will destroy.
        Handle _owned { this->Coroutine() };
    };

    /// @copydoc Task::operator co_await
    Awaiter operator co_await() && noexcept
    {
        return Awaiter { std::exchange(_handle, {}) };
    }

  private:
    /// The owned coroutine frame, or null when empty / moved-from.
    Handle _handle {};
};

namespace Detail
{

    /// Builds the `Task<T>` that owns this promise's coroutine frame.
    /// Defined out-of-line because it needs the complete `Task<T>` type.
    /// @return A `Task<T>` wrapping the handle reconstructed from `*this`.
    template <typename T>
    Task<T> TaskPromise<T>::get_return_object() noexcept
    {
        return Task<T> { std::coroutine_handle<TaskPromise<T>>::from_promise(*this) };
    }

    /// Builds the `Task<void>` that owns this promise's coroutine frame.
    /// @return A `Task<void>` wrapping the handle reconstructed from
    ///         `*this`.
    inline Task<void> TaskPromise<void>::get_return_object() noexcept
    {
        return Task<void> { std::coroutine_handle<TaskPromise<void>>::from_promise(*this) };
    }

} // namespace Detail

} // namespace Coro
