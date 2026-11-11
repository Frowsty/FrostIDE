// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession: one terminal tab's shell (ADR 0051), a pseudo terminal (IPty) and its screen model
// (VtScreen), driven by three dedicated threads (PLAN.md section 3.3): the reader feeds the child's
// output to the screen, the writer sends input to the child, and the waiter reports the child's exit.
/*
 * Threads and locking. The screen is guarded by the session's mutex: the reader holds it while it
 * parses, and the input functions (any thread, usually the GUI) hold it while libvterm encodes; read()
 * runs a function on the screen under it. The bytes the screen produces go to the writer's queue, so
 * no thread ever blocks on the child while holding the mutex, and a child that stops reading its
 * input never stalls the reader.
 *
 * Notifications. The observer functions run on the reader thread (changed, titleChanged, bell) or the
 * waiter thread (exited), outside the mutex; they must not block and must not call close() (a GUI
 * posts them to its own thread). `changed` is coalesced: it fires once until acknowledge() is called,
 * so a flood of output produces one notification per repaint.
 *
 * Lifetime. close() (or destruction) ends the process tree and joins the threads; no observer call
 * runs after it returns. The launcher must outlive start().
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/term/keys.hpp"
#include "frost/term/pty.hpp"
#include "frost/term/vt_screen.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace frost::term {

struct SessionObserver {
    // The screen changed (coalesced until acknowledge()).
    std::function<void()> changed{};
    std::function<void(const std::string& title)> titleChanged{};
    std::function<void()> bell{};
    // The shell exited (its tree may still be running until close()).
    std::function<void(io::ExitStatus status)> exited{};
};

class TerminalSession {
public:
    /*
     * Opens a terminal of `size` with `launcher` and starts the threads. Errors: those of
     * IPtyLauncher::open.
     */
    [[nodiscard]] static Result<std::unique_ptr<TerminalSession>> start(IPtyLauncher& launcher,
                                                                        const io::ProcessSpec& spec, PtySize size,
                                                                        std::size_t scrollbackLines,
                                                                        SessionObserver observer);
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete ("owns threads that refer to it");
    TerminalSession& operator=(const TerminalSession&) = delete ("owns threads that refer to it");

    // Runs `function` with the screen under the session's mutex and returns its result. Must not call
    // back into the session.
    template<class Function>
    decltype(auto) read(Function&& function) const {
        LockGuard lock(m_mutex);
        return std::forward<Function>(function)(static_cast<const VtScreen&>(m_screen));
    }

    // --- Input (thread-safe; the bytes are queued for the writer) ---

    bool sendWin32Key(const Win32KeyEvent& event);
    void sendKey(TermKey key, KeyMods mods);
    void sendChar(char32_t character, KeyMods mods);
    void sendText(std::string_view utf8);
    void paste(std::string_view utf8);
    void mouseMove(int row, int column, KeyMods mods);
    void mouseButton(MouseButton button, bool pressed, int row, int column, KeyMods mods);
    void focusChanged(bool focused);
    // Raw bytes to the child (tests; programs that need an exact sequence).
    void sendBytes(std::string_view bytes);

    // Resizes the screen and the terminal. Errors: those of IPty::resize.
    [[nodiscard]] Result<void> resize(PtySize size);

    // Re-arms the coalesced `changed` notification (the GUI calls it when it repaints).
    void acknowledge() noexcept { m_changePending.store(false, std::memory_order_release); }

    [[nodiscard]] io::ProcessId pid() const noexcept { return m_pid; }
    // The shell's exit status once it exited.
    [[nodiscard]] std::optional<io::ExitStatus> exitStatus() const;
    [[nodiscard]] bool running() const { return !exitStatus().has_value(); }
    // The processes still in the shell's container (see IPty::containedProcesses).
    [[nodiscard]] Result<std::vector<io::ContainedProcess>> containedProcesses();
    // Waits until the visible screen's text contains `needle` (tests). False on timeout.
    [[nodiscard]] bool waitForText(std::string_view needle, IClock::Duration timeout) const;

    // Ends the process tree and joins the threads (see the header comment). Idempotent.
    void close();

private:
    TerminalSession(std::unique_ptr<IPty> pty, PtySize size, std::size_t scrollbackLines, SessionObserver observer);
    void readerLoop();
    void writerLoop();
    void waiterLoop();
    // Moves the screen's outgoing bytes to the writer queue. Called with m_mutex held.
    void flushOutputLocked() FROST_REQUIRES(m_mutex);
    void enqueue(const std::string& bytes);

    std::unique_ptr<IPty> m_pty;
    io::ProcessId m_pid = 0;
    SessionObserver m_observer;

    mutable Mutex m_mutex;
    mutable std::condition_variable m_screenChanged;
    VtScreen m_screen FROST_GUARDED_BY(m_mutex);
    std::optional<io::ExitStatus> m_exit FROST_GUARDED_BY(m_mutex);

    Mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::string m_queue FROST_GUARDED_BY(m_queueMutex);
    bool m_stopWriter FROST_GUARDED_BY(m_queueMutex) = false;

    std::atomic<bool> m_changePending{false};
    std::atomic<bool> m_closed{false};
    std::thread m_reader;
    std::thread m_writer;
    std::thread m_waiter;
};

} // namespace frost::term
