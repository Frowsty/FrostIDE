// SPDX-License-Identifier: Apache-2.0
//
// Platform-independent parts of frost::term's PTY layer (frost/term/pty.hpp, frost/term/fake_pty.hpp):
// ptyProcessSpec and the FakePty double.

#include "frost/term/fake_pty.hpp"
#include "frost/term/pty.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace frost::term {

io::ProcessSpec ptyProcessSpec(std::string executable, std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = std::move(executable);
    spec.arguments = std::move(arguments);
    spec.stdinMode = io::Stdio::Inherit;
    spec.stdoutMode = io::Stdio::Inherit;
    spec.stderrMode = io::Stdio::Inherit;
    spec.killOnClose = true;
    return spec;
}

// --- FakePty ---

Result<void> FakePty::write(std::string_view bytes) {
    UniqueLock lock(m_mutex);
    if (m_closed) {
        return fail(ErrorCode::IoError, "the fake terminal is closed");
    }
    m_input.append(bytes);
    m_changed.notify_all();
    return {};
}

Result<std::size_t> FakePty::read(std::span<char> buffer) {
    UniqueLock lock(m_mutex);
    while (m_output.empty() && !m_closed && !m_eof) {
        m_changed.wait(lock.native());
    }
    const std::size_t count = std::min(buffer.size(), m_output.size());
    std::copy_n(m_output.data(), count, buffer.data());
    m_output.erase(0, count);
    return count;
}

Result<void> FakePty::resize(PtySize size) {
    if (size.columns == 0 || size.rows == 0) {
        return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
    }
    LockGuard lock(m_mutex);
    m_size = size;
    ++m_resizes;
    return {};
}

Result<io::ExitStatus> FakePty::wait(std::optional<IClock::Duration> timeout) {
    UniqueLock lock(m_mutex);
    const auto deadline =
        timeout ? std::chrono::steady_clock::now() + *timeout : std::chrono::steady_clock::time_point::max();
    while (!m_exit) {
        if (!timeout) {
            m_changed.wait(lock.native());
        } else if (m_changed.wait_until(lock.native(), deadline) == std::cv_status::timeout && !m_exit) {
            return fail(ErrorCode::Timeout, "the fake terminal's child is still running");
        }
    }
    return *m_exit;
}

Result<std::vector<io::ContainedProcess>> FakePty::containedProcesses() {
    LockGuard lock(m_mutex);
    if (m_exit) {
        return std::vector<io::ContainedProcess>{};
    }
    return std::vector<io::ContainedProcess>{io::ContainedProcess{.pid = kPid, .imageName = "fake-shell"}};
}

void FakePty::close() noexcept {
    LockGuard lock(m_mutex);
    m_closed = true;
    if (!m_exit) {
        m_exit = io::ExitStatus{.exitCode = io::kTerminatedExitCode, .signal = 0};
    }
    m_changed.notify_all();
}

void FakePty::print(std::string_view bytes) {
    LockGuard lock(m_mutex);
    m_output.append(bytes);
    m_changed.notify_all();
}

void FakePty::exit(int exitCode) {
    LockGuard lock(m_mutex);
    m_exit = io::ExitStatus{.exitCode = exitCode, .signal = 0};
    m_eof = true;
    m_changed.notify_all();
}

std::string FakePty::input() const {
    LockGuard lock(m_mutex);
    return m_input;
}

bool FakePty::waitForInput(std::string_view needle, IClock::Duration timeout) const {
    UniqueLock lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_input.find(needle) == std::string::npos) {
        if (m_changed.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return m_input.find(needle) != std::string::npos;
        }
    }
    return true;
}

PtySize FakePty::size() const {
    LockGuard lock(m_mutex);
    return m_size;
}

int FakePty::resizeCount() const {
    LockGuard lock(m_mutex);
    return m_resizes;
}

bool FakePty::closed() const {
    LockGuard lock(m_mutex);
    return m_closed;
}

// --- FakePtyLauncher ---

Result<std::unique_ptr<IPty>> FakePtyLauncher::open(const io::ProcessSpec& spec, PtySize size) {
    if (size.columns == 0 || size.rows == 0) {
        return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
    }
    LockGuard lock(m_mutex);
    if (m_failNext) {
        m_failNext = false;
        return fail(ErrorCode::NotFound, "the fake launcher was told to fail");
    }
    auto pty = std::make_shared<FakePty>(size);
    m_specs.push_back(spec);
    m_ptys.push_back(pty);
    // The session owns a forwarding handle; the test keeps the shared state through m_ptys.
    return std::unique_ptr<IPty>(std::make_unique<FakePtyHandle>(std::move(pty)));
}

std::vector<io::ProcessSpec> FakePtyLauncher::specs() const {
    LockGuard lock(m_mutex);
    return m_specs;
}

std::shared_ptr<FakePty> FakePtyLauncher::pty(std::size_t index) const {
    LockGuard lock(m_mutex);
    return index < m_ptys.size() ? m_ptys[index] : nullptr;
}

std::size_t FakePtyLauncher::count() const {
    LockGuard lock(m_mutex);
    return m_ptys.size();
}

void FakePtyLauncher::failNext() {
    LockGuard lock(m_mutex);
    m_failNext = true;
}

} // namespace frost::term
