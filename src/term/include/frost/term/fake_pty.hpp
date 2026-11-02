// SPDX-License-Identifier: Apache-2.0
//
// FakePtyLauncher and FakePty: the deterministic double of frost::term's PTY seam (frost/term/pty.hpp),
// for tests of the session and the UI. Nothing is started: a test feeds the "child's" output with
// print(), ends it with exit(), and reads what the terminal sent with input().
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/term/pty.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::term {

/*
 * The shared state of one fake terminal. The session reads and writes it through the IPty the
 * launcher returned (a FakePtyHandle); the test keeps a shared_ptr. Thread-safe.
 */
class FakePty {
public:
    // The pid every fake terminal reports.
    static constexpr io::ProcessId kPid = 4242;

    explicit FakePty(PtySize size) : m_size(size) {}

    // IPty's operations (FakePtyHandle forwards to them).
    [[nodiscard]] Result<void> write(std::string_view bytes);
    [[nodiscard]] Result<std::size_t> read(std::span<char> buffer);
    [[nodiscard]] Result<void> resize(PtySize size);
    [[nodiscard]] Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout);
    [[nodiscard]] Result<std::vector<io::ContainedProcess>> containedProcesses();
    // Ends the fake child with kTerminatedExitCode (unless it exited) and the output.
    void close() noexcept;

    // Test side: output the child "wrote", then the child's exit (which also ends the output).
    void print(std::string_view bytes);
    void exit(int exitCode);
    // Everything written to the terminal's input so far.
    [[nodiscard]] std::string input() const;
    // Waits until input() contains `needle`; false on timeout.
    [[nodiscard]] bool waitForInput(std::string_view needle, IClock::Duration timeout) const;
    [[nodiscard]] PtySize size() const;
    [[nodiscard]] int resizeCount() const;
    [[nodiscard]] bool closed() const;

private:
    mutable Mutex m_mutex;
    mutable std::condition_variable m_changed;
    PtySize m_size FROST_GUARDED_BY(m_mutex);
    int m_resizes FROST_GUARDED_BY(m_mutex) = 0;
    std::string m_input FROST_GUARDED_BY(m_mutex);
    std::string m_output FROST_GUARDED_BY(m_mutex);
    std::optional<io::ExitStatus> m_exit FROST_GUARDED_BY(m_mutex);
    bool m_eof FROST_GUARDED_BY(m_mutex) = false;
    bool m_closed FROST_GUARDED_BY(m_mutex) = false;
};

// The IPty over a FakePty; destruction closes it.
class FakePtyHandle final : public IPty {
public:
    explicit FakePtyHandle(std::shared_ptr<FakePty> pty) : m_pty(std::move(pty)) {}
    ~FakePtyHandle() override { m_pty->close(); }
    FakePtyHandle(const FakePtyHandle&) = delete ("a terminal has exactly one owner");
    FakePtyHandle& operator=(const FakePtyHandle&) = delete ("a terminal has exactly one owner");

    [[nodiscard]] io::ProcessId pid() const noexcept override { return FakePty::kPid; }
    [[nodiscard]] Result<void> write(std::string_view bytes) override { return m_pty->write(bytes); }
    [[nodiscard]] Result<std::size_t> read(std::span<char> buffer) override { return m_pty->read(buffer); }
    [[nodiscard]] Result<void> resize(PtySize size) override { return m_pty->resize(size); }
    [[nodiscard]] Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        return m_pty->wait(timeout);
    }
    [[nodiscard]] Result<std::vector<io::ContainedProcess>> containedProcesses() override {
        return m_pty->containedProcesses();
    }
    void close() noexcept override { m_pty->close(); }

private:
    std::shared_ptr<FakePty> m_pty;
};

// Records every open() and hands out FakePty terminals. Thread-safe.
class FakePtyLauncher final : public IPtyLauncher {
public:
    [[nodiscard]] Result<std::unique_ptr<IPty>> open(const io::ProcessSpec& spec, PtySize size) override;

    // The specs open() was called with, in order.
    [[nodiscard]] std::vector<io::ProcessSpec> specs() const;
    // The index-th terminal opened, or null.
    [[nodiscard]] std::shared_ptr<FakePty> pty(std::size_t index) const;
    [[nodiscard]] std::size_t count() const;
    // The next open() fails with NotFound.
    void failNext();

private:
    mutable Mutex m_mutex;
    std::vector<io::ProcessSpec> m_specs FROST_GUARDED_BY(m_mutex);
    std::vector<std::shared_ptr<FakePty>> m_ptys FROST_GUARDED_BY(m_mutex);
    bool m_failNext FROST_GUARDED_BY(m_mutex) = false;
};

} // namespace frost::term
