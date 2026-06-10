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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Coro
{

namespace Detail
{

    /// Slot for a single child task in a WhenAll. Holds the child task
    /// (so its frame lives until the WhenAll resumes) plus the result
    /// (or exception) it produced.
    template <typename T>
    struct WhenAllSlot
    {
        /// The child task's result type (`void` for value-less children).
        using Value = T;
        using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;

        Task<T> task;
        Stored result {};
        std::exception_ptr exception {};
    };

    /// Shared state between the parent (the WhenAll awaiter) and all
    /// child wrapper coroutines. Single-threaded; the counter is a
    /// plain integer, no atomics needed.
    struct WhenAllState
    {
        IScheduler* scheduler { nullptr };
        std::coroutine_handle<> parent { std::noop_coroutine() };
        std::size_t remaining { 0 };

        /// Called by a child wrapper when the child task completes.
        /// Decrements the counter and, on the last completion, posts
        /// the parent to the scheduler so it can collect results.
        void OnChildDone() noexcept
        {
            if (--remaining == 0)
                scheduler->Post(parent);
        }
    };

    /// Per-child wrapper coroutine. Awaits the child task, captures its
    /// result/exception into the slot, then signals the shared state.
    /// Returns a `Task<void>` so the wrapper itself owns its frame.
    ///
    /// The slot and state are passed by pointer (not reference): they
    /// live inside the awaitable, which outlives every child wrapper, so
    /// the indirection is a borrow with a guaranteed lifetime — pointers
    /// keep the borrow explicit and side-step the coroutine
    /// reference-parameter pitfall.
    /// @param slot Borrowed slot holding this child's task and result.
    /// @param state Borrowed shared completion state.
    template <typename T>
    inline Task<void> WhenAllChild(WhenAllSlot<T>* slot, WhenAllState* state)
    {
        try
        {
            if constexpr (std::is_void_v<T>)
            {
                co_await std::move(slot->task);
            }
            else
            {
                slot->result.emplace(co_await std::move(slot->task));
            }
        }
        catch (...)
        {
            slot->exception = std::current_exception();
        }
        state->OnChildDone();
    }

} // namespace Detail

/// Awaitable produced by `WhenAll(scheduler, t1, t2, ...)`. Suspends
/// the parent until every child task has completed, then resumes the
/// parent with a `std::tuple` of the children's results.
///
/// Children run concurrently on the same single-threaded scheduler:
/// each child is wrapped in a forwarding coroutine and posted to the
/// scheduler's ready queue. When a child suspends (e.g. on `Sleep`)
/// the next ready child runs, achieving overlap without threads.
template <typename... Ts>
class WhenAllAwaitable
{
  public:
    WhenAllAwaitable(IScheduler& scheduler, Task<Ts>&&... tasks):
        _state { std::make_unique<Detail::WhenAllState>() },
        _slots { std::make_unique<std::tuple<Detail::WhenAllSlot<Ts>...>>(
            Detail::WhenAllSlot<Ts> { std::move(tasks), {}, {} }...) }
    {
        _state->scheduler = &scheduler;
        _state->remaining = sizeof...(Ts);
    }

    WhenAllAwaitable(WhenAllAwaitable const&) = delete;
    WhenAllAwaitable& operator=(WhenAllAwaitable const&) = delete;
    WhenAllAwaitable(WhenAllAwaitable&&) noexcept = default;
    WhenAllAwaitable& operator=(WhenAllAwaitable&&) noexcept = default;
    ~WhenAllAwaitable() = default;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return sizeof...(Ts) == 0;
    }

    void await_suspend(std::coroutine_handle<> continuation)
    {
        _state->parent = continuation;
        // Spawn every child wrapper. We must keep the wrapper Tasks
        // alive until they complete — they self-drive once posted,
        // because each wrapper awaits its slot task and then signals
        // the shared state.
        SpawnChildren(std::index_sequence_for<Ts...> {});
    }

    /// Collect the children's results into a tuple. Void children are
    /// folded out of the returned tuple type by `MakeReturn`, so they
    /// contribute completion (and exceptions) but no tuple element.
    auto await_resume()
    {
        return MakeReturn(std::index_sequence_for<Ts...> {});
    }

  private:
    template <std::size_t... Is>
    void SpawnChildren(std::index_sequence<Is...> /*indices*/)
    {
        // Build a wrapper Task per child, store it so its frame stays
        // alive until completion, and post it to the scheduler.
        _wrappers.reserve(sizeof...(Ts));
        ((_wrappers.push_back(Detail::WhenAllChild(&std::get<Is>(*_slots), _state.get())),
          _state->scheduler->Post(_wrappers.back().Native())),
         ...);
    }

    /// Build the tuple of return values, propagating any exception
    /// from the first slot that holds one. Each child contributes a
    /// tuple piece — a one-element tuple for value children, an empty
    /// tuple for void children — and `std::tuple_cat` folds the voids
    /// out, so `WhenAll(Task<int>, Task<void>, Task<string>)` resolves
    /// to `tuple<int, string>` rather than including a `monostate`.
    template <std::size_t... Is>
    auto MakeReturn(std::index_sequence<Is...> /*indices*/)
    {
        (RethrowIfFailed<Is>(), ...);
        return std::tuple_cat(ExtractValue<Is>()...);
    }

    template <std::size_t I>
    void RethrowIfFailed()
    {
        auto& slot = std::get<I>(*_slots);
        if (slot.exception)
            std::rethrow_exception(slot.exception);
    }

    /// @return This child's contribution to the result tuple: a
    ///         one-element tuple holding its value, or an empty tuple
    ///         for a `Task<void>` child (folded away by `tuple_cat`).
    template <std::size_t I>
    auto ExtractValue()
    {
        using SlotType = std::tuple_element_t<I, std::tuple<Detail::WhenAllSlot<Ts>...>>;
        if constexpr (std::is_void_v<typename SlotType::Value>)
            return std::tuple<> {};
        else
        {
            auto& slot = std::get<I>(*_slots);
            // The slot is always engaged here: a non-void child either
            // emplaced its result or stored an exception that
            // `RethrowIfFailed` already re-threw before we extract. The
            // explicit `has_value` guard documents that invariant and
            // keeps the access checked.
            assert(slot.result.has_value());
            return std::tuple<typename SlotType::Value> { std::move(slot.result).value() };
        }
    }

    std::unique_ptr<Detail::WhenAllState> _state;
    std::unique_ptr<std::tuple<Detail::WhenAllSlot<Ts>...>> _slots;
    std::vector<Task<void>> _wrappers;
};

/// Construct a WhenAll awaitable.
/// @param scheduler Scheduler that drives the children concurrently.
/// @param tasks Child tasks to run. Ownership is moved into the awaitable.
/// @return Awaitable; on `co_await`, resumes with a tuple of results.
template <typename... Ts>
[[nodiscard]] inline auto WhenAll(IScheduler& scheduler, Task<Ts>&&... tasks)
{
    return WhenAllAwaitable<Ts...> { scheduler, std::move(tasks)... };
}

} // namespace Coro
