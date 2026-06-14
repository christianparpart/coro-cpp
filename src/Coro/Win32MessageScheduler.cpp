// SPDX-License-Identifier: Apache-2.0
#include <Coro/Win32MessageScheduler.hpp>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <algorithm>
#include <cassert>
#include <chrono>

#include <windows.h>

namespace Coro
{

namespace Detail
{
    /// Bridge giving the file-local window procedure access to the
    /// scheduler's private timer machinery without widening the public
    /// API (the procedure's signature needs <windows.h> types, so it
    /// cannot be declared a friend in the header).
    struct Win32MessageSchedulerAccess
    {
        /// Forward a `WM_TIMER` dispatch to @p self's timer driver.
        static void OnTimer(Win32MessageScheduler& self)
        {
            self._driver.OnTimerExpired();
        }
    };
} // namespace Detail

namespace
{
    /// Private window message carrying a coroutine handle to resume in
    /// its `LPARAM` (the handle's `address()`).
    constexpr UINT ResumeMessage = WM_APP + 0;

    /// Identifier of the single `SetTimer` slot tracking the earliest
    /// pending deadline.
    constexpr UINT_PTR TimerSlot = 1;

    /// Window class shared by every scheduler instance in the process.
    constexpr wchar_t const* WindowClassName = L"CoroWin32MessageScheduler";

    /// @return `GetLastError()` wrapped as a system-category error code.
    [[nodiscard]] std::error_code LastError() noexcept
    {
        return std::error_code { static_cast<int>(GetLastError()), std::system_category() };
    }

    /// Window procedure of the hidden message-only window: resumes
    /// posted continuations and forwards timer ticks to the owning
    /// scheduler (found via `GWLP_USERDATA`).
    LRESULT CALLBACK SchedulerWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
            case ResumeMessage:
                std::coroutine_handle<>::from_address(reinterpret_cast<void*>(lparam)).resume();
                return 0;
            case WM_TIMER:
                if (wparam == TimerSlot)
                {
                    // GWLP_USERDATA is cleared in the destructor, so a
                    // straggling WM_TIMER after teardown is a no-op.
                    auto* const self = reinterpret_cast<Win32MessageScheduler*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                    if (self != nullptr)
                        Detail::Win32MessageSchedulerAccess::OnTimer(*self);
                    return 0;
                }
                break;
            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    /// Register the shared window class. Idempotent; the class stays
    /// registered for the process lifetime, since other live scheduler
    /// instances (or ones created later) keep using it.
    /// @return `true` when the class is registered (now or already).
    [[nodiscard]] bool RegisterWindowClass() noexcept
    {
        auto windowClass = WNDCLASSEXW {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &SchedulerWndProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = WindowClassName;
        if (RegisterClassExW(&windowClass) != 0)
            return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
} // namespace

std::expected<std::unique_ptr<Win32MessageScheduler>, std::error_code> Win32MessageScheduler::Create(IClock& clock)
{
    if (!RegisterWindowClass())
        return std::unexpected { LastError() };

    auto* const window =
        CreateWindowExW(0, WindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
        return std::unexpected { LastError() };

    // Private constructor — std::make_unique cannot reach it.
    auto scheduler =
        std::unique_ptr<Win32MessageScheduler> { new Win32MessageScheduler { clock, window, GetCurrentThreadId() } };
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(scheduler.get()));
    return scheduler;
}

Win32MessageScheduler::Win32MessageScheduler(IClock& clock, void* window, std::uint32_t threadId) noexcept:
    _clock { clock },
    _window { window },
    _threadId { threadId },
    _driver {
        clock,
        // Resume seam: post the handle as a WM_APP message (thread-safe).
        [this](std::coroutine_handle<> handle) { Post(handle); },
        // Arm seam: re-use the single SetTimer slot, clamped to the Win32
        // valid range. Re-using the same slot id replaces the previous
        // timer, so exactly one OS timer is live regardless of how many
        // entries are queued.
        [this](std::chrono::milliseconds delay) {
            auto const interval = std::clamp(static_cast<long long>(delay.count()),
                                             static_cast<long long>(USER_TIMER_MINIMUM),
                                             static_cast<long long>(USER_TIMER_MAXIMUM));
            SetTimer(static_cast<HWND>(_window), TimerSlot, static_cast<UINT>(interval), nullptr);
        },
        // Cancel seam.
        [this] { KillTimer(static_cast<HWND>(_window), TimerSlot); },
    }
{
}

Win32MessageScheduler::~Win32MessageScheduler()
{
    assert(GetCurrentThreadId() == _threadId && "Coro::Win32MessageScheduler must be destroyed on its pump thread");
    auto* const window = static_cast<HWND>(_window);
    KillTimer(window, TimerSlot);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);

    // Drop — do not resume, do not destroy — resume messages still in the
    // queue. The frames behind them belong to their owners (a Task or an
    // awaiter up the chain); destroying here would double-free them. This
    // mirrors EventLoop teardown, which likewise drops unfired timer
    // handles. (Entries still parked in the driver's TimerQueue are dropped
    // the same way when the _driver member is destroyed.)
    auto pending = MSG {};
    while (PeekMessageW(&pending, window, ResumeMessage, ResumeMessage, PM_REMOVE) != 0)
    {
        // intentionally empty — the message is discarded
    }

    DestroyWindow(window);
    // The window class intentionally stays registered: it is shared
    // process-wide and re-registration on the next Create is a no-op.
}

void Win32MessageScheduler::Post(std::coroutine_handle<> handle)
{
    auto const posted =
        PostMessageW(static_cast<HWND>(_window), ResumeMessage, 0, reinterpret_cast<LPARAM>(handle.address()));
    // PostMessage only fails when the thread's message queue is exhausted
    // (10'000 messages by default) — at that point the continuation is
    // lost, which is unrecoverable for the coroutine graph. Fail loudly.
    assert(posted != 0 && "Coro::Win32MessageScheduler::Post: message queue exhausted; continuation lost");
    static_cast<void>(posted);
}

void Win32MessageScheduler::ScheduleAt(IClock::TimePoint when, std::coroutine_handle<> handle)
{
    assert(GetCurrentThreadId() == _threadId && "Coro::Win32MessageScheduler::ScheduleAt is pump-thread-only");
    // The shared driver owns the due/park/re-arm logic; this class only
    // supplies the Win32 transport (see the constructor).
    _driver.ScheduleAt(when, handle);
}

IClock const& Win32MessageScheduler::Clock() const noexcept
{
    return _clock;
}

std::size_t Win32MessageScheduler::PendingTimerCount() const noexcept
{
    return _driver.PendingCount();
}

} // namespace Coro
