// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Coro/Scheduler.hpp>
#include <Coro/Task.hpp>

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace Coro
{

namespace Detail
{

    /// Sentinel "no winner yet" value for `WhenAnyState::winner`.
    inline constexpr std::size_t NoWinner = static_cast<std::size_t>(-1);

    /// Shared state for WhenAny — tracks which child completed first.
    template <typename T>
    struct WhenAnyState
    {
        IScheduler* scheduler { nullptr };
        std::coroutine_handle<> parent { std::noop_coroutine() };
        std::size_t winner { NoWinner };
        std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> result {};
        std::exception_ptr exception {};

        /// Called by a child wrapper on completion. The first one to
        /// arrive wins, captures the result, and posts the parent.
        /// Later completions are dropped on the floor.
        /// @param index Index of the completing child.
        /// @param value The child's value, if it produced one.
        /// @param exc Exception captured from the child, if it threw.
        void OnChildDone(std::size_t index, std::optional<T>&& value, std::exception_ptr exc) noexcept
            requires(!std::is_void_v<T>)
        {
            if (winner != NoWinner)
                return;
            winner = index;
            result = std::move(value);
            exception = std::move(exc);
            scheduler->Post(parent);
        }

        /// @param index Index of the completing child.
        /// @param exc Exception captured from the child, if it threw.
        void OnChildDone(std::size_t index, std::exception_ptr exc) noexcept
            requires(std::is_void_v<T>)
        {
            if (winner != NoWinner)
                return;
            winner = index;
            exception = std::move(exc);
            scheduler->Post(parent);
        }
    };

    template <typename T>
    inline Task<void> WhenAnyChild(Task<T> task, std::shared_ptr<WhenAnyState<T>> state, std::size_t index)
    {
        try
        {
            if constexpr (std::is_void_v<T>)
            {
                co_await std::move(task);
                state->OnChildDone(index, nullptr);
            }
            else
            {
                auto value = co_await std::move(task);
                state->OnChildDone(index, std::optional<T> { std::move(value) }, nullptr);
            }
        }
        catch (...)
        {
            if constexpr (std::is_void_v<T>)
                state->OnChildDone(index, std::current_exception());
            else
                state->OnChildDone(index, std::nullopt, std::current_exception());
        }
    }

} // namespace Detail

/// Result of a `WhenAny` await: the index of the child that completed
/// first, plus its value (if any).
template <typename T>
struct WhenAnyResult
{
    std::size_t index { 0 };
    [[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, T> value {};
};

/// Awaitable produced by `WhenAny(scheduler, tasks)`. Suspends the
/// parent until the first child task completes, then resumes the
/// parent with a `WhenAnyResult` carrying the winner's index and value.
///
/// The remaining children continue running on the scheduler until they
/// complete — there is no in-flight cancellation in this lightning-talk
/// scaffold. (A production WhenAny would propagate a cancellation
/// token; here we keep the seam minimal so the talk can focus on the
/// `co_await` ergonomics rather than cancellation propagation.)
template <typename T>
class WhenAnyAwaitable
{
  public:
    WhenAnyAwaitable(IScheduler& scheduler, std::vector<Task<T>>&& tasks):
        _state { std::make_shared<Detail::WhenAnyState<T>>() },
        _tasks { std::move(tasks) }
    {
        _state->scheduler = &scheduler;
    }

    [[nodiscard]] bool await_ready() const noexcept
    {
        return _tasks.empty();
    }

    void await_suspend(std::coroutine_handle<> continuation)
    {
        _state->parent = continuation;
        _wrappers.reserve(_tasks.size());
        for (std::size_t i = 0; i < _tasks.size(); ++i)
        {
            _wrappers.push_back(Detail::WhenAnyChild<T>(std::move(_tasks[i]), _state, i));
            _state->scheduler->Post(_wrappers.back().Native());
        }
    }

    WhenAnyResult<T> await_resume()
    {
        if (_state->exception)
            std::rethrow_exception(_state->exception);
        WhenAnyResult<T> out;
        out.index = _state->winner;
        if constexpr (!std::is_void_v<T>)
        {
            // The winner always stored a value before posting the
            // parent (the throwing path rethrows above), so the optional
            // is engaged. The explicit `has_value` guard documents that
            // invariant and keeps the access checked.
            assert(_state->result.has_value());
            out.value = std::move(_state->result.value());
        }
        return out;
    }

  private:
    std::shared_ptr<Detail::WhenAnyState<T>> _state;
    std::vector<Task<T>> _tasks;
    std::vector<Task<void>> _wrappers;
};

/// Construct a WhenAny awaitable from a homogeneous vector of tasks.
/// @param scheduler Scheduler driving the children concurrently.
/// @param tasks Child tasks; ownership is moved.
/// @return Awaitable; on `co_await`, resumes with the winner's index
///         and value.
template <typename T>
[[nodiscard]] inline auto WhenAny(IScheduler& scheduler, std::vector<Task<T>>&& tasks)
{
    return WhenAnyAwaitable<T> { scheduler, std::move(tasks) };
}

} // namespace Coro
