// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession (frost/term/session.hpp): the reader, writer and waiter threads around an IPty and
// a VtScreen.

#include "frost/term/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace frost::term {

namespace {

// Lines of scrollback waitForText searches besides the visible screen.
constexpr int kSearchScrollback = 200;

} // namespace

TerminalSession::TerminalSession(std::unique_ptr<IPty> pty, PtySize size, std::size_t scrollbackLines,
                                 SessionObserver observer)
    : m_pty(std::move(pty)), m_pid(m_pty->pid()), m_observer(std::move(observer)), m_screen(size, scrollbackLines) {}

Result<std::unique_ptr<TerminalSession>> TerminalSession::start(IPtyLauncher& launcher, const io::ProcessSpec& spec,
                                                                PtySize size, std::size_t scrollbackLines,
                                                                SessionObserver observer) {
    auto pty = launcher.open(spec, size);
    if (!pty) {
        return std::unexpected(std::move(pty.error()));
    }
    std::unique_ptr<TerminalSession> session(
        new TerminalSession(std::move(*pty), size, scrollbackLines, std::move(observer)));
    TerminalSession* self = session.get();
    self->m_reader = std::thread([self] { self->readerLoop(); });
    self->m_writer = std::thread([self] { self->writerLoop(); });
    self->m_waiter = std::thread([self] { self->waiterLoop(); });
    return session;
}

TerminalSession::~TerminalSession() { close(); }

void TerminalSession::close() {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // The tree first: a pending read then ends, a blocked write fails and the wait returns.
    m_pty->close();
    {
        LockGuard lock(m_queueMutex);
        m_stopWriter = true;
        m_queueChanged.notify_all();
    }
    for (std::thread* thread : {&m_reader, &m_writer, &m_waiter}) {
        if (thread->joinable()) {
            thread->join();
        }
    }
}

void TerminalSession::readerLoop() {
    std::array<char, 64 * 1024> chunk{};
    for (;;) {
        auto count = m_pty->read(chunk);
        if (!count || *count == 0) {
            break;
        }
        ScreenEvents events;
        std::string title;
        {
            LockGuard lock(m_mutex);
            m_screen.feed(std::string_view(chunk.data(), *count));
            events = m_screen.takeEvents();
            if (events.titleChanged) {
                title = m_screen.title();
            }
            flushOutputLocked();
            m_screenChanged.notify_all();
        }
        if (events.changed && m_observer.changed && !m_changePending.exchange(true, std::memory_order_acq_rel)) {
            m_observer.changed();
        }
        if (events.titleChanged && m_observer.titleChanged) {
            m_observer.titleChanged(title);
        }
        for (int bell = 0; bell < events.bells && m_observer.bell; ++bell) {
            m_observer.bell();
        }
    }
}

void TerminalSession::writerLoop() {
    for (;;) {
        std::string chunk;
        {
            UniqueLock lock(m_queueMutex);
            while (m_queue.empty() && !m_stopWriter) {
                m_queueChanged.wait(lock.native());
            }
            if (m_stopWriter) {
                return; // Closing: what is still queued is dropped.
            }
            chunk.swap(m_queue);
        }
        // A failed write means the terminal is gone; later input is dropped the same way.
        static_cast<void>(m_pty->write(chunk));
    }
}

void TerminalSession::waiterLoop() {
    auto status = m_pty->wait(std::nullopt);
    if (!status) {
        return;
    }
    {
        LockGuard lock(m_mutex);
        m_exit = *status;
        m_screenChanged.notify_all();
    }
    if (!m_closed.load(std::memory_order_acquire) && m_observer.exited) {
        m_observer.exited(*status);
    }
}

void TerminalSession::flushOutputLocked() {
    std::string bytes = m_screen.takeOutput();
    if (!bytes.empty()) {
        enqueue(std::move(bytes));
    }
}

void TerminalSession::enqueue(std::string bytes) {
    LockGuard lock(m_queueMutex);
    if (m_stopWriter) {
        return;
    }
    m_queue += bytes;
    m_queueChanged.notify_all();
}

bool TerminalSession::sendWin32Key(const Win32KeyEvent& event) {
    LockGuard lock(m_mutex);
    const bool consumed = m_screen.sendWin32Key(event);
    flushOutputLocked();
    return consumed;
}

void TerminalSession::sendKey(TermKey key, KeyMods mods) {
    LockGuard lock(m_mutex);
    m_screen.sendKey(key, mods);
    flushOutputLocked();
}

void TerminalSession::sendChar(char32_t character, KeyMods mods) {
    LockGuard lock(m_mutex);
    m_screen.sendChar(character, mods);
    flushOutputLocked();
}

void TerminalSession::sendText(std::string_view utf8) {
    LockGuard lock(m_mutex);
    m_screen.sendText(utf8);
    flushOutputLocked();
}

void TerminalSession::paste(std::string_view utf8) {
    LockGuard lock(m_mutex);
    m_screen.paste(utf8);
    flushOutputLocked();
}

void TerminalSession::mouseMove(int row, int column, KeyMods mods) {
    LockGuard lock(m_mutex);
    m_screen.mouseMove(row, column, mods);
    flushOutputLocked();
}

void TerminalSession::mouseButton(MouseButton button, bool pressed, int row, int column, KeyMods mods) {
    LockGuard lock(m_mutex);
    m_screen.mouseButton(button, pressed, row, column, mods);
    flushOutputLocked();
}

void TerminalSession::focusChanged(bool focused) {
    LockGuard lock(m_mutex);
    m_screen.focusChanged(focused);
    flushOutputLocked();
}

void TerminalSession::sendBytes(std::string_view bytes) {
    // Under the screen's mutex too, so the bytes keep their order with encoded input.
    LockGuard lock(m_mutex);
    flushOutputLocked();
    enqueue(std::string(bytes));
}

Result<void> TerminalSession::resize(PtySize size) {
    {
        LockGuard lock(m_mutex);
        m_screen.resize(size);
        flushOutputLocked();
    }
    return m_pty->resize(size);
}

std::optional<io::ExitStatus> TerminalSession::exitStatus() const {
    LockGuard lock(m_mutex);
    return m_exit;
}

Result<std::vector<io::ContainedProcess>> TerminalSession::containedProcesses() { return m_pty->containedProcesses(); }

bool TerminalSession::waitForText(std::string_view needle, IClock::Duration timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(m_mutex);
    for (;;) {
        const int back = std::min(static_cast<int>(m_screen.scrollbackSize()), kSearchScrollback);
        const std::string text =
            m_screen.text(ScreenPos{-back, 0}, ScreenPos{m_screen.rows() - 1, m_screen.columns() - 1});
        if (text.find(needle) != std::string::npos) {
            return true;
        }
        if (m_screenChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            const std::string last =
                m_screen.text(ScreenPos{-back, 0}, ScreenPos{m_screen.rows() - 1, m_screen.columns() - 1});
            return last.find(needle) != std::string::npos;
        }
    }
}

} // namespace frost::term
