// SPDX-License-Identifier: Apache-2.0
//
// Demo-only input seam: a non-blocking "was a key pressed?" probe.
// Lives in demos/ rather than the public Coro library because terminal
// I/O is a concern of the demo harness, not the coroutine primitives.
//
// Per the AGENT.md dependency-injection rule, the coroutines that react
// to a keypress NEVER call `::read()` / `termios` directly: they reach
// the terminal through the abstract `IKeyPress` interface. Tests (and a
// scripted talk) can inject a fake that "presses a key" on a schedule,
// with no real terminal involved.

#pragma once

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace demos
{

/// Abstract, non-blocking keyboard probe.
///
/// The single virtual is deliberately tiny: a cooperative poller asks
/// "has the user pressed anything since I last looked?" once per tick.
/// This keeps the seam trivial to fake — an injected implementation can
/// return `true` after N polls to simulate a keypress at a fixed time.
class IKeyPress
{
  public:
    IKeyPress() = default;
    IKeyPress(IKeyPress const&) = delete;
    IKeyPress& operator=(IKeyPress const&) = delete;
    IKeyPress(IKeyPress&&) = delete;
    IKeyPress& operator=(IKeyPress&&) = delete;
    virtual ~IKeyPress() = default;

    /// @return true if at least one key has been pressed since the
    ///         previous call (consuming the pending input), false if no
    ///         input is currently available.
    [[nodiscard]] virtual bool Pressed() = 0;
};

/// Real terminal implementation of `IKeyPress`.
///
/// Puts the controlling terminal into cbreak + non-blocking mode for
/// its lifetime (RAII: the original `termios` and file-status flags are
/// restored in the destructor, even on early return), so a single byte
/// can be read without waiting for Enter and without blocking the
/// cooperative event loop.
class TerminalKeyPress final: public IKeyPress
{
  public:
    /// Switch the terminal into raw, non-blocking mode.
    /// @param fd File descriptor to probe; defaults to stdin.
    explicit TerminalKeyPress(int fd = STDIN_FILENO):
        _fd { fd },
        _saved { CurrentTermios(fd) },
        _savedFlags { ::fcntl(fd, F_GETFL, 0) }
    {
        auto raw = _saved;
        raw.c_lflag = raw.c_lflag & ~(static_cast<tcflag_t>(ICANON | ECHO));
        ::tcsetattr(_fd, TCSANOW, &raw);
        ::fcntl(_fd, F_SETFL, _savedFlags | O_NONBLOCK);
    }

    TerminalKeyPress(TerminalKeyPress const&) = delete;
    TerminalKeyPress& operator=(TerminalKeyPress const&) = delete;
    TerminalKeyPress(TerminalKeyPress&&) = delete;
    TerminalKeyPress& operator=(TerminalKeyPress&&) = delete;

    /// Restore the terminal to its original line-buffered, blocking mode.
    ~TerminalKeyPress() override
    {
        ::tcsetattr(_fd, TCSANOW, &_saved);
        ::fcntl(_fd, F_SETFL, _savedFlags);
    }

    [[nodiscard]] bool Pressed() override
    {
        char buf = 0;
        auto pressed = false;
        // Drain whatever bytes are buffered so a held key or an escape
        // sequence counts as a single "pressed" rather than queueing up.
        while (::read(_fd, &buf, 1) > 0)
            pressed = true;
        return pressed;
    }

  private:
    /// Snapshot the current terminal attributes for `fd`, used to seed
    /// `_saved` in the member-initializer list so it can be restored
    /// verbatim in the destructor.
    /// @param fd File descriptor to query.
    /// @return The terminal's current `termios` settings.
    [[nodiscard]] static ::termios CurrentTermios(int fd)
    {
        auto attrs = ::termios {};
        ::tcgetattr(fd, &attrs);
        return attrs;
    }

    int _fd;
    ::termios _saved;
    int _savedFlags;
};

} // namespace demos
