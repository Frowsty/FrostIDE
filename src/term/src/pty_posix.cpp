// SPDX-License-Identifier: Apache-2.0
//
// The Linux pseudo terminal (frost/term/pty.hpp): posix_openpt for the master, the slave device opened
// by the child itself (io's ProcessSpec::pseudoTerminal), a dedicated reader thread, and a wake pipe
// so close() can interrupt a reader or writer blocked in poll().
/*
 * Closing. The master is non-blocking; the reader and write() wait in poll() on the master and the
 * wake pipe. close() writes to the wake pipe, joins the reader, takes the write lock (a writer left
 * poll() with an error), then closes the master: the kernel hangs the terminal up, which sends SIGHUP
 * to the session's foreground process group and its leader. Every process still in the session (found
 * in /proc/<pid>/stat, field 6) then gets SIGKILL, and the child's process group is terminated through
 * io (which also covers a kernel without /proc). The leader is never reaped before the IProcess goes,
 * so its pid, which is the session id, cannot be reused meanwhile (io's reaping rule, ADR 0003).
 */

#include "frost/io/os_error.hpp"
#include "frost/term/pty.hpp"

#include "pty_buffer.hpp"
#include <sys/ioctl.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace frost::term {

namespace {

// An owned file descriptor.
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : m_fd(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete ("a descriptor has one owner");
    Fd& operator=(const Fd&) = delete ("a descriptor has one owner");

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }
    void reset() noexcept {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = -1;
    }

private:
    int m_fd = -1;
};

Result<void> setWindowSize(int master, PtySize size) {
    winsize window{};
    window.ws_col = size.columns;
    window.ws_row = size.rows;
    if (::ioctl(master, TIOCSWINSZ, &window) != 0) {
        return std::unexpected(io::errnoError(errno, "ioctl(TIOCSWINSZ)", "terminal"));
    }
    return {};
}

// The session id in /proc/<pid>/stat: field 6, counted after the ')' that ends the command name.
std::optional<long> sessionOf(const char* statPath) {
    Fd file(::open(statPath, O_RDONLY | O_CLOEXEC));
    if (!file.valid()) {
        return std::nullopt;
    }
    std::array<char, 1024> buffer{};
    const ssize_t count = ::read(file.get(), buffer.data(), buffer.size() - 1);
    if (count <= 0) {
        return std::nullopt;
    }
    const std::string_view text(buffer.data(), static_cast<std::size_t>(count));
    const std::size_t paren = text.rfind(')');
    if (paren == std::string_view::npos) {
        return std::nullopt;
    }
    // After ") ": state, ppid, pgrp, session.
    std::string_view rest = text.substr(paren + 1);
    for (int field = 0; field < 4; ++field) {
        const std::size_t start = rest.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        rest.remove_prefix(start);
        if (field == 3) {
            long session = 0;
            const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), session);
            if (error != std::errc{}) {
                return std::nullopt;
            }
            return session;
        }
        const std::size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
            return std::nullopt;
        }
        rest.remove_prefix(space);
    }
    return std::nullopt;
}

// SIGKILL to every process of session `session` but this one. Best effort.
void killSession(pid_t session) noexcept {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) {
        return;
    }
    const pid_t self = ::getpid();
    while (const dirent* entry = ::readdir(proc)) {
        long pid = 0;
        const std::string_view name(entry->d_name);
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (error != std::errc{} || end != name.data() + name.size() || pid == self) {
            continue;
        }
        std::array<char, 64> path{};
        const auto written = std::format_to_n(path.data(), path.size() - 1, "/proc/{}/stat", pid);
        *written.out = '\0';
        if (sessionOf(path.data()) == static_cast<long>(session)) {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
    ::closedir(proc);
}

class PosixPty final : public IPty {
public:
    PosixPty(Fd master, Fd wakeRead, Fd wakeWrite, std::unique_ptr<io::IProcess> process)
        : m_master(std::move(master)), m_wakeRead(std::move(wakeRead)), m_wakeWrite(std::move(wakeWrite)),
          m_process(std::move(process)) {
        m_masterFd = m_master.get();
        m_reader = std::thread([this] { drain(); });
    }
    ~PosixPty() override { close(); }
    PosixPty(const PosixPty&) = delete ("a terminal has exactly one owner");
    PosixPty& operator=(const PosixPty&) = delete ("a terminal has exactly one owner");

    [[nodiscard]] io::ProcessId pid() const noexcept override { return m_process->pid(); }

    Result<void> write(std::string_view bytes) override {
        LockGuard lock(m_writeMutex);
        while (!bytes.empty()) {
            if (m_closing.load(std::memory_order_acquire) || !m_master.valid()) {
                return fail(ErrorCode::IoError, "the terminal is closed");
            }
            const ssize_t written = ::write(m_master.get(), bytes.data(), bytes.size());
            if (written > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(written));
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && errno != EAGAIN) {
                return std::unexpected(io::errnoError(errno, "write", "terminal input"));
            }
            std::array<pollfd, 2> fds{pollfd{m_master.get(), POLLOUT, 0}, pollfd{m_wakeRead.get(), POLLIN, 0}};
            if (::poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR) {
                return std::unexpected(io::errnoError(errno, "poll", "terminal input"));
            }
            if ((fds[1].revents & POLLIN) != 0) {
                return fail(ErrorCode::IoError, "the terminal is closed");
            }
            if ((fds[0].revents & (POLLERR | POLLHUP)) != 0 && (fds[0].revents & POLLOUT) == 0) {
                return fail(ErrorCode::IoError, "the terminal was hung up");
            }
        }
        return {};
    }

    Result<std::size_t> read(std::span<char> buffer) override { return m_buffer.take(buffer); }

    Result<void> resize(PtySize size) override {
        if (size.columns == 0 || size.rows == 0) {
            return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
        }
        LockGuard lock(m_writeMutex);
        if (!m_master.valid()) {
            return fail(ErrorCode::IoError, "the terminal is closed");
        }
        return setWindowSize(m_master.get(), size);
    }

    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override { return m_process->wait(timeout); }

    Result<std::vector<io::ContainedProcess>> containedProcesses() override { return m_process->containedProcesses(); }

    void close() noexcept override {
        if (m_closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_buffer.abandon();
        const char wake = 1;
        static_cast<void>(::write(m_wakeWrite.get(), &wake, 1));
        if (m_reader.joinable()) {
            m_reader.join();
        }
        {
            LockGuard lock(m_writeMutex);
            m_master.reset(); // The hang-up: SIGHUP to the session.
        }
        killSession(static_cast<pid_t>(m_process->pid()));
        static_cast<void>(m_process->terminate());
    }

private:
    // The reader thread: copies the output into m_buffer until EIO or EOF (every holder of the slave
    // closed it) or the wake pipe fires (close()).
    void drain() noexcept {
        std::array<char, 64 * 1024> chunk{};
        for (;;) {
            std::array<pollfd, 2> fds{pollfd{m_masterFd, POLLIN, 0}, pollfd{m_wakeRead.get(), POLLIN, 0}};
            if (::poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((fds[1].revents & POLLIN) != 0) {
                break;
            }
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const ssize_t count = ::read(m_masterFd, chunk.data(), chunk.size());
            if (count > 0) {
                static_cast<void>(m_buffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(count))));
                continue;
            }
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            break; // 0 or EIO: the slave side is gone.
        }
        m_buffer.finish();
    }

    Mutex m_writeMutex;
    Fd m_master FROST_GUARDED_BY(m_writeMutex);
    // The master's number for the reader thread, which only reads; close() resets m_master after joining it.
    int m_masterFd = -1;
    Fd m_wakeRead;
    Fd m_wakeWrite;
    std::unique_ptr<io::IProcess> m_process;
    detail::OutputBuffer m_buffer;
    std::atomic<bool> m_closing{false};
    std::thread m_reader;
};

} // namespace

Result<std::unique_ptr<IPty>> RealPtyLauncher::open(const io::ProcessSpec& spec, PtySize size) {
    if (size.columns == 0 || size.rows == 0) {
        return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
    }
    if (spec.pseudoTerminal) {
        return fail(ErrorCode::InvalidArgument, "the PTY launcher sets ProcessSpec::pseudoTerminal itself");
    }
    Fd master(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
    if (!master.valid()) {
        return std::unexpected(io::errnoError(errno, "posix_openpt"));
    }
    if (::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0) {
        return std::unexpected(io::errnoError(errno, "grantpt/unlockpt"));
    }
    std::array<char, 128> name{};
    if (const int rc = ::ptsname_r(master.get(), name.data(), name.size()); rc != 0) {
        return std::unexpected(io::errnoError(rc, "ptsname_r"));
    }
    if (auto sized = setWindowSize(master.get(), size); !sized) {
        return std::unexpected(std::move(sized.error()));
    }
    std::array<int, 2> wake{-1, -1};
    if (::pipe2(wake.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(io::errnoError(errno, "pipe2"));
    }
    Fd wakeRead(wake[0]);
    Fd wakeWrite(wake[1]);

    io::ProcessSpec attached = spec;
    attached.pseudoTerminal = io::PseudoTerminal{.windowsPseudoConsole = nullptr, .posixTerminalPath = name.data()};
    auto process = m_launcher->launch(attached);
    if (!process) {
        return std::unexpected(std::move(process.error()));
    }
    return std::make_unique<PosixPty>(std::move(master), std::move(wakeRead), std::move(wakeWrite),
                                      std::move(*process));
}

} // namespace frost::term
