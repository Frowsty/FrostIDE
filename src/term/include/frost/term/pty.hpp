// SPDX-License-Identifier: Apache-2.0
//
// The pseudo-terminal seam of frost::term (PLAN.md section 2 "Terminal", ADR 0051): IPty, one running
// child on a pseudo terminal, and IPtyLauncher, which creates the terminal and starts the child on it.
/*
 * Real implementation. Windows: bundled ConPTY, loaded beside the executable, with a logged system
 * ConPTY fallback. Linux:
 * posix_openpt/grantpt/unlockpt and the slave device by path. Either way the child starts through
 * io::IProcessLauncher with ProcessSpec::pseudoTerminal set, so the absolute-path rule, environment
 * handling and containment are io's (a Job Object with kill-on-close on Windows; a new session on
 * Linux). A dedicated reader thread per terminal drains the terminal's output into a buffer, so the
 * terminal can always be closed (ConPTY's close waits for its output to be read) and read() never
 * races close().
 *
 * Process tree. close() ends everything the session started: Windows terminates the Job Object and
 * closes the pseudo console; Linux hangs the terminal up (SIGHUP to the session), then sends SIGKILL to
 * every process still in the child's session (read from /proc), the child's process group included.
 * A descendant that started a session of its own (a daemon) is not ended, as with io's containment.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace frost::term {

// A terminal size in character cells. Both at least 1.
struct PtySize {
    std::uint16_t columns = 80;
    std::uint16_t rows = 24;

    friend bool operator==(const PtySize&, const PtySize&) = default;
};

/*
 * One child running on a pseudo terminal. Owned through std::unique_ptr; destruction calls close().
 * Thread safety: write(), read(), resize(), wait() and close() may be called from different threads
 * concurrently (the session's reader, writer and UI threads do); two concurrent read() calls are not
 * allowed.
 */
class IPty {
public:
    virtual ~IPty() = default;
    IPty(const IPty&) = delete ("a terminal has exactly one owner");
    IPty& operator=(const IPty&) = delete ("a terminal has exactly one owner");

    [[nodiscard]] virtual io::ProcessId pid() const noexcept = 0;
    // Sends `bytes` to the terminal's input, as typed (the child reads them from its stdin). Blocks
    // while the input buffer is full. Errors: IoError (the terminal is closed or broken).
    [[nodiscard]] virtual Result<void> write(std::string_view bytes) = 0;
    // Waits until output is available and copies up to buffer.size() bytes; 0 means end of output
    // (every process holding the terminal exited, or close() ran). Errors: IoError.
    [[nodiscard]] virtual Result<std::size_t> read(std::span<char> buffer) = 0;
    // Changes the terminal size; the child gets SIGWINCH (Linux) or a console resize (Windows).
    // Errors: InvalidArgument (a zero dimension), IoError.
    [[nodiscard]] virtual Result<void> resize(PtySize size) = 0;
    // Waits for the child (not its descendants) to exit; forever when `timeout` is nullopt.
    // Errors: Timeout, IoError.
    [[nodiscard]] virtual Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout = std::nullopt) = 0;
    // The processes still running in the child's container (see io::IProcess::containedProcesses).
    [[nodiscard]] virtual Result<std::vector<io::ContainedProcess>> containedProcesses() = 0;
    // Ends the process tree and closes the terminal (see the header comment); a blocked read()
    // returns the output still buffered, then 0. Idempotent, never fails.
    virtual void close() noexcept = 0;

protected:
    IPty() = default;
};

/*
 * Creates pseudo terminals. `spec` is the child to start: its pseudoTerminal field must be empty (the
 * launcher sets it), its streams Stdio::Inherit, killOnClose set. Errors: InvalidArgument (a zero
 * dimension, a spec io::validateProcessSpec rejects), Unsupported (no ConPTY: Windows before 10 1809),
 * the launcher's errors, IoError. Thread-safe.
 */
class IPtyLauncher {
public:
    [[nodiscard]] virtual Result<std::unique_ptr<IPty>> open(const io::ProcessSpec& spec, PtySize size) = 0;

protected:
    constexpr IPtyLauncher() noexcept = default;
    constexpr IPtyLauncher(const IPtyLauncher&) noexcept = default;
    constexpr IPtyLauncher& operator=(const IPtyLauncher&) noexcept = default;
    ~IPtyLauncher() = default;
};

// The real launcher over `launcher` (which must outlive it): ConPTY on Windows, posix_openpt on Linux.
class RealPtyLauncher final : public IPtyLauncher {
public:
    explicit RealPtyLauncher(io::IProcessLauncher& launcher = io::realProcessLauncher()) noexcept
        : m_launcher(&launcher) {}
    [[nodiscard]] Result<std::unique_ptr<IPty>> open(const io::ProcessSpec& spec, PtySize size) override;

private:
    io::IProcessLauncher* m_launcher;
};

// A spec suitable for open(): `executable` with `arguments`, streams Inherit, killOnClose.
[[nodiscard]] io::ProcessSpec ptyProcessSpec(std::string executable, std::vector<std::string> arguments = {});

} // namespace frost::term
