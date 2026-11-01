// SPDX-License-Identifier: Apache-2.0
//
// The real IProcessLauncher on POSIX (Linux, glibc >= 2.34): posix_spawn with
// O_CLOEXEC pipes, an optional new session, waitid/pidfd for waiting.
//
/*
 * Reaping. The child is reaped (waitpid) only when the IProcess is destroyed.
 * Until then wait() learns the exit status with waitid(WNOWAIT), which leaves
 * a zombie that keeps the pid, and with killOnClose the process-group id,
 * reserved. That makes terminate() and the destructor's group kill immune to
 * pid reuse: they can never signal an unrelated process.
 *
 * SIGPIPE. Writing to a pipe whose reader is gone raises SIGPIPE, which kills
 * the process by default. Rather than changing the process-wide disposition,
 * each write blocks SIGPIPE in the calling thread, and if the write failed
 * with EPIPE, consumes the signal it generated (sigtimedwait with a zero
 * timeout) before restoring the mask; the error is reported as EPIPE.
 */

#include "frost/core/mutex.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "internal.hpp"
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <format>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <unistd.h>

// The process environment (POSIX names it here; glibc also declares it in <unistd.h> under _GNU_SOURCE).
extern char** environ;

namespace frost::io {
namespace {

using detail::UniqueFd;

// The communicate() read chunk in bytes.
constexpr std::size_t kChunkBytes = 64 * 1024;
// How long communicate() waits for a terminated process to exit before returning Timeout.
constexpr auto kReapGrace = std::chrono::seconds(5);

// poll() timeout in milliseconds: rounded up, clamped to int.
int toPollMs(IClock::Duration duration) noexcept {
    if (duration <= IClock::Duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(duration).count();
    return static_cast<int>(std::min<long long>(ms, 1 << 30));
}

// write() that cannot raise SIGPIPE (see the file comment). Returns the byte count, or -1
// with the errno value in `error`.
ssize_t writeWithoutSigpipe(int fd, const char* data, std::size_t size, int& error) noexcept {
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    sigset_t previous;
    pthread_sigmask(SIG_BLOCK, &pipeSet, &previous);
    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    const ssize_t done = ::write(fd, data, size);
    error = done < 0 ? errno : 0;
    if (done < 0 && error == EPIPE && !alreadyPending) {
        const timespec zero{};
        while (sigtimedwait(&pipeSet, nullptr, &zero) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return done;
}

// Waits until `fd` is ready for `events` (forever). Returns 0 or an errno value.
int pollOne(int fd, short events) noexcept {
    pollfd entry{fd, events, 0};
    for (;;) {
        if (::poll(&entry, 1, -1) >= 0) {
            return 0;
        }
        if (errno != EINTR) {
            return errno;
        }
    }
}

ExitStatus statusFromSiginfo(const siginfo_t& info) noexcept {
    if (info.si_code == CLD_EXITED) {
        return ExitStatus{info.si_status, 0};
    }
    return ExitStatus{128 + info.si_status, info.si_status}; // CLD_KILLED or CLD_DUMPED.
}

// A pidfd for `pid` (Linux >= 5.3), or an invalid descriptor where unsupported.
UniqueFd openPidfd(pid_t pid) noexcept {
#ifdef SYS_pidfd_open
    return UniqueFd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
#else
    static_cast<void>(pid);
    return UniqueFd();
#endif
}

class PosixProcess final : public IProcess {
public:
    PosixProcess(pid_t pid, bool ownGroup, UniqueFd in, UniqueFd out, UniqueFd err)
        : m_pid(pid), m_ownGroup(ownGroup), m_pidfd(openPidfd(pid)), m_in(std::move(in)), m_out(std::move(out)),
          m_err(std::move(err)) {}

    ~PosixProcess() override {
        m_in.reset();
        m_out.reset();
        m_err.reset();
        if (m_ownGroup) {
            // The zombie (or live) leader keeps the group id reserved, so this reaches only our tree.
            ::kill(-m_pid, SIGKILL);
            reap(0);
        } else {
            // Not contained: reap if finished, otherwise let it run.
            // TODO(M1): the reactor's child watcher reaps detached children when they exit.
            reap(WNOHANG);
        }
    }

    ProcessId pid() const noexcept override { return static_cast<ProcessId>(m_pid); }

    Result<void> writeStdin(std::string_view bytes) override {
        if (!m_in.valid()) {
            return fail(ErrorCode::InvalidArgument, "stdin is not an open pipe");
        }
        while (!bytes.empty()) {
            int error = 0;
            const ssize_t done = writeWithoutSigpipe(m_in.get(), bytes.data(), bytes.size(), error);
            if (done >= 0) {
                bytes.remove_prefix(static_cast<std::size_t>(done));
                continue;
            }
            if (error == EAGAIN) {
                if (const int pollError = pollOne(m_in.get(), POLLOUT); pollError != 0) {
                    return std::unexpected(errnoError(pollError, "writeStdin: poll"));
                }
                continue;
            }
            if (error == EINTR) {
                continue;
            }
            if (error == EPIPE) {
                return fail(ErrorCode::IoError, "writeStdin: the child closed its stdin");
            }
            return std::unexpected(errnoError(error, "writeStdin"));
        }
        return {};
    }

    void closeStdin() noexcept override { m_in.reset(); }

    Result<std::size_t> readStdout(std::span<char> buffer) override {
        return blockingRead(m_out, buffer, "readStdout");
    }
    Result<std::size_t> readStderr(std::span<char> buffer) override {
        return blockingRead(m_err, buffer, "readStderr");
    }

    Result<ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        {
            LockGuard lock(m_mutex);
            if (m_status) {
                return *m_status;
            }
        }
        siginfo_t info{};
        if (!timeout) {
            // Blocks until exit without reaping (WNOWAIT).
            while (::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOWAIT) != 0) {
                if (errno != EINTR) {
                    return std::unexpected(errnoError(errno, "waitid"));
                }
            }
            return remember(statusFromSiginfo(info));
        }
        const auto deadline = detail::deadlineAfter(*timeout);
        auto pause = std::chrono::milliseconds(1);
        for (;;) {
            info = siginfo_t{};
            if (::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOWAIT | WNOHANG) != 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(errnoError(errno, "waitid"));
            }
            if (info.si_pid != 0) {
                return remember(statusFromSiginfo(info));
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= IClock::Duration::zero()) {
                return fail(ErrorCode::Timeout, std::format("process {} is still running", m_pid));
            }
            if (m_pidfd.valid()) {
                // A pidfd becomes readable when the process exits.
                pollfd entry{m_pidfd.get(), POLLIN, 0};
                static_cast<void>(::poll(&entry, 1, toPollMs(left)));
            } else {
                // No pidfd (old kernel): poll with backoff up to 50 ms.
                std::this_thread::sleep_for(std::min<IClock::Duration>(pause, left));
                pause = std::min(pause * 2, std::chrono::milliseconds(50));
            }
        }
    }

    Result<void> terminate() override {
        // The child is never reaped before destruction, so m_pid cannot have been reused.
        const pid_t target = m_ownGroup ? -m_pid : m_pid;
        if (::kill(target, SIGKILL) != 0 && errno != ESRCH) {
            return std::unexpected(errnoError(errno, "kill"));
        }
        return {};
    }

    Result<ProcessResult> communicate(std::string_view stdinData, IClock::Duration timeout) override;

private:
    static Result<std::size_t> blockingRead(UniqueFd& fd, std::span<char> buffer, std::string_view operation) {
        if (!fd.valid()) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", operation));
        }
        for (;;) {
            const ssize_t got = ::read(fd.get(), buffer.data(), buffer.size());
            if (got >= 0) {
                return static_cast<std::size_t>(got);
            }
            if (errno == EAGAIN) {
                if (const int pollError = pollOne(fd.get(), POLLIN); pollError != 0) {
                    return std::unexpected(errnoError(pollError, operation));
                }
            } else if (errno != EINTR) {
                return std::unexpected(errnoError(errno, operation));
            }
        }
    }

    ExitStatus remember(ExitStatus status) {
        LockGuard lock(m_mutex);
        m_status = status;
        return status;
    }

    // waitpid with `options` (0 or WNOHANG), retried on EINTR; errors are ignored (destructor use).
    void reap(int options) noexcept {
        int status = 0;
        while (::waitpid(m_pid, &status, options) < 0 && errno == EINTR) {
        }
    }

    const pid_t m_pid;
    const bool m_ownGroup;
    UniqueFd m_pidfd;
    UniqueFd m_in;
    UniqueFd m_out;
    UniqueFd m_err;
    Mutex m_mutex;
    std::optional<ExitStatus> m_status FROST_GUARDED_BY(m_mutex);
};

/*
 * communicate() multiplexes the three non-blocking pipe ends with poll(), so a
 * child that fills stdout while we still feed stdin (or fills both output
 * pipes) cannot deadlock us.
 */
Result<ProcessResult> PosixProcess::communicate(std::string_view stdinData, IClock::Duration timeout) {
    if (!stdinData.empty() && !m_in.valid()) {
        return fail(ErrorCode::InvalidArgument, "communicate: stdin data given but stdin is not an open pipe");
    }
    const auto deadline = detail::deadlineAfter(timeout);
    const auto timedOut = [&]() -> Result<ProcessResult> {
        static_cast<void>(terminate());
        static_cast<void>(wait(kReapGrace));
        return fail(ErrorCode::Timeout,
                    std::format("process {} did not finish within {} ms and was terminated", m_pid,
                                std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
    };

    ProcessResult result;
    bool writing = m_in.valid() && !stdinData.empty();
    if (!writing) {
        closeStdin();
    }
    bool readingOut = m_out.valid();
    bool readingErr = m_err.valid();
    std::size_t inOffset = 0;
    std::vector<char> buffer(kChunkBytes);

    // Reads what is available from one pipe; returns false at EOF.
    const auto drain = [&](int fd, std::string& sink, std::string_view what) -> Result<bool> {
        for (;;) {
            const ssize_t got = ::read(fd, buffer.data(), buffer.size());
            if (got > 0) {
                sink.append(buffer.data(), static_cast<std::size_t>(got));
                return true;
            }
            if (got == 0) {
                return false;
            }
            if (errno == EAGAIN) {
                return true;
            }
            if (errno != EINTR) {
                return std::unexpected(errnoError(errno, what));
            }
        }
    };

    while (writing || readingOut || readingErr) {
        std::array<pollfd, 3> entries{};
        nfds_t count = 0;
        int outIndex = -1;
        int errIndex = -1;
        int inIndex = -1;
        if (readingOut) {
            outIndex = static_cast<int>(count);
            entries[count++] = pollfd{m_out.get(), POLLIN, 0};
        }
        if (readingErr) {
            errIndex = static_cast<int>(count);
            entries[count++] = pollfd{m_err.get(), POLLIN, 0};
        }
        if (writing) {
            inIndex = static_cast<int>(count);
            entries[count++] = pollfd{m_in.get(), POLLOUT, 0};
        }
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= IClock::Duration::zero()) {
            return timedOut();
        }
        const int ready = ::poll(entries.data(), count, toPollMs(left));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errnoError(errno, "communicate: poll"));
        }
        if (ready == 0) {
            return timedOut();
        }
        // POLLHUP/POLLERR on a read end still need a read to collect the tail and see EOF.
        if (outIndex >= 0 && entries[static_cast<std::size_t>(outIndex)].revents != 0) {
            auto more = drain(m_out.get(), result.stdoutBytes, "communicate: read stdout");
            if (!more) {
                return std::unexpected(std::move(more.error()));
            }
            readingOut = *more;
        }
        if (errIndex >= 0 && entries[static_cast<std::size_t>(errIndex)].revents != 0) {
            auto more = drain(m_err.get(), result.stderrBytes, "communicate: read stderr");
            if (!more) {
                return std::unexpected(std::move(more.error()));
            }
            readingErr = *more;
        }
        if (inIndex >= 0) {
            const short events = entries[static_cast<std::size_t>(inIndex)].revents;
            if ((events & (POLLERR | POLLHUP)) != 0) {
                writing = false; // The child closed stdin early; the rest of the data is dropped.
            } else if ((events & POLLOUT) != 0) {
                int error = 0;
                const std::size_t chunk = std::min(stdinData.size() - inOffset, kChunkBytes);
                const ssize_t done = writeWithoutSigpipe(m_in.get(), stdinData.data() + inOffset, chunk, error);
                if (done >= 0) {
                    inOffset += static_cast<std::size_t>(done);
                    writing = inOffset < stdinData.size();
                } else if (error == EPIPE) {
                    writing = false;
                } else if (error != EAGAIN && error != EINTR) {
                    return std::unexpected(errnoError(error, "communicate: write stdin"));
                }
            }
            if (!writing) {
                closeStdin();
            }
        }
    }

    auto status = wait(deadline - std::chrono::steady_clock::now());
    if (!status) {
        if (status.error().code() == ErrorCode::Timeout) {
            return timedOut();
        }
        return std::unexpected(std::move(status.error()));
    }
    result.exitCode = status->exitCode;
    result.signal = status->signal;
    return result;
}

// One end pair of a pipe: parent's end (non-blocking) and the child's end.
struct PipeEnds {
    UniqueFd parent;
    UniqueFd child;
};

// Moves `fd` to a number above 2 if it landed on a standard descriptor (possible when this
// process closed one), so the child's dup2 actions onto 0-2 cannot clobber it.
Result<UniqueFd> aboveStdio(UniqueFd fd) {
    if (fd.get() > STDERR_FILENO) {
        return fd;
    }
    const int moved = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        return std::unexpected(errnoError(errno, "fcntl(F_DUPFD_CLOEXEC)"));
    }
    return UniqueFd(moved);
}

Result<PipeEnds> createPipe(bool parentReads) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return std::unexpected(errnoError(errno, "pipe2"));
    }
    UniqueFd rawRead(fds[0]);
    UniqueFd rawWrite(fds[1]);
    auto readEnd = aboveStdio(std::move(rawRead));
    if (!readEnd) {
        return std::unexpected(std::move(readEnd.error()));
    }
    auto writeEnd = aboveStdio(std::move(rawWrite));
    if (!writeEnd) {
        return std::unexpected(std::move(writeEnd.error()));
    }
    PipeEnds ends{parentReads ? std::move(*readEnd) : std::move(*writeEnd),
                  parentReads ? std::move(*writeEnd) : std::move(*readEnd)};
    const int flags = ::fcntl(ends.parent.get(), F_GETFL);
    if (flags < 0 || ::fcntl(ends.parent.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        return std::unexpected(errnoError(errno, "fcntl(O_NONBLOCK)"));
    }
    return ends;
}

// RAII for the posix_spawn attribute objects.
struct SpawnActions {
    posix_spawn_file_actions_t actions;
    SpawnActions() { posix_spawn_file_actions_init(&actions); }
    ~SpawnActions() { posix_spawn_file_actions_destroy(&actions); }
    SpawnActions(const SpawnActions&) = delete ("owns a posix_spawn_file_actions_t");
    SpawnActions& operator=(const SpawnActions&) = delete ("owns a posix_spawn_file_actions_t");
};

struct SpawnAttributes {
    posix_spawnattr_t attributes;
    SpawnAttributes() { posix_spawnattr_init(&attributes); }
    ~SpawnAttributes() { posix_spawnattr_destroy(&attributes); }
    SpawnAttributes(const SpawnAttributes&) = delete ("owns a posix_spawnattr_t");
    SpawnAttributes& operator=(const SpawnAttributes&) = delete ("owns a posix_spawnattr_t");
};

/*
 * Whether standard descriptor `fd` would reach a child. A slot this process
 * closed is soon reused by descriptors frost::io opens itself (a pidfd, a pipe
 * end of a concurrent launch), all of them close-on-exec, so "open" is not
 * enough: a descriptor with FD_CLOEXEC vanishes at exec and leaves the child's
 * slot empty, and the child's next open() would become its stdout.
 */
bool reachesChild(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) == 0;
}

/*
 * What Stdio::Inherit means for standard descriptor `target` (see process.hpp): the
 * null device when this process has no inheritable descriptor there, and for a
 * terminal stdin of a `contained` child. That child leads its own session, outside
 * the terminal's job control, so it could read the terminal while another job (the
 * shell that started a backgrounded IDE) is in the foreground, splitting the user's
 * keystrokes, a typed password included. Terminal stdout and stderr are kept.
 */
Stdio effectiveMode(Stdio mode, int target, bool contained) noexcept {
    if (mode != Stdio::Inherit) {
        return mode;
    }
    if (!reachesChild(target) || (target == STDIN_FILENO && contained && ::isatty(target) == 1)) {
        return Stdio::Null;
    }
    return mode;
}

// Adds the file action for one standard stream. `pipe` is the child end for Stdio::Pipe.
int addStreamAction(posix_spawn_file_actions_t& actions, Stdio mode, int target, const UniqueFd& pipe) noexcept {
    switch (mode) {
    case Stdio::Pipe: return posix_spawn_file_actions_adddup2(&actions, pipe.get(), target);
    case Stdio::Null:
        return posix_spawn_file_actions_addopen(&actions, target, "/dev/null", target == 0 ? O_RDONLY : O_WRONLY, 0);
    case Stdio::MergeIntoStdout: return posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, target);
    case Stdio::Inherit: return 0;
    }
    return EINVAL;
}

class PosixProcessLauncher final : public IProcessLauncher {
public:
    constexpr PosixProcessLauncher() noexcept = default;

    Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) override {
        if (auto valid = validateProcessSpec(spec, /*windowsRules=*/false); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        // Decided before any pipe is created, since a new pipe may take a free descriptor 0-2.
        const Stdio stdinMode = effectiveMode(spec.stdinMode, STDIN_FILENO, spec.killOnClose);
        const Stdio stdoutMode = effectiveMode(spec.stdoutMode, STDOUT_FILENO, spec.killOnClose);
        const Stdio stderrMode = effectiveMode(spec.stderrMode, STDERR_FILENO, spec.killOnClose);
        PipeEnds in;
        PipeEnds out;
        PipeEnds err;
        for (auto [mode, ends, parentReads] : {std::tuple{stdinMode, &in, false}, std::tuple{stdoutMode, &out, true},
                                               std::tuple{stderrMode, &err, true}}) {
            if (mode == Stdio::Pipe) {
                auto pipe = createPipe(parentReads);
                if (!pipe) {
                    return std::unexpected(std::move(pipe.error()));
                }
                *ends = std::move(*pipe);
            }
        }

        SpawnActions actions;
        int rc = addStreamAction(actions.actions, stdinMode, STDIN_FILENO, in.child);
        if (rc == 0) {
            rc = addStreamAction(actions.actions, stdoutMode, STDOUT_FILENO, out.child);
        }
        if (rc == 0) {
            rc = addStreamAction(actions.actions, stderrMode, STDERR_FILENO, err.child);
        }
        // Belt and braces: descriptors other code opened without O_CLOEXEC are not inherited either.
        if (rc == 0) {
            rc = posix_spawn_file_actions_addclosefrom_np(&actions.actions, 3);
        }
        if (rc == 0 && !spec.workingDirectory.empty()) {
            rc = posix_spawn_file_actions_addchdir_np(&actions.actions, spec.workingDirectory.c_str());
        }
        if (rc != 0) {
            return std::unexpected(errnoError(rc, "posix_spawn_file_actions"));
        }

        // The child starts with no blocked signals and default handlers, whatever this process uses.
        SpawnAttributes attributes;
        sigset_t none;
        sigemptyset(&none);
        sigset_t all;
        sigfillset(&all);
        int flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
        if (spec.killOnClose) {
            /*
             * A new session (glibc >= 2.26), which also makes the child the leader of
             * a new process group whose id is its pid, so kill(-pid) reaches the tree.
             * Unlike a mere new group, the session has no controlling terminal: a
             * child that opens /dev/tty (ssh and git prompts) gets ENXIO and fails
             * fast, instead of being stopped by SIGTTIN as a background job of the
             * IDE's terminal while wait() blocks.
             */
            flags |= POSIX_SPAWN_SETSID;
        }
        rc = posix_spawnattr_setsigmask(&attributes.attributes, &none);
        if (rc == 0) {
            rc = posix_spawnattr_setsigdefault(&attributes.attributes, &all);
        }
        if (rc == 0) {
            rc = posix_spawnattr_setflags(&attributes.attributes, static_cast<short>(flags));
        }
        if (rc != 0) {
            return std::unexpected(errnoError(rc, "posix_spawnattr"));
        }

        // argv and envp point into these strings, which outlive the posix_spawn call.
        std::vector<std::string> argvStorage;
        argvStorage.reserve(spec.arguments.size() + 1);
        argvStorage.push_back(spec.executable);
        argvStorage.insert(argvStorage.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argv;
        for (std::string& argument : argvStorage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        std::vector<std::string> envStorage;
        if (!spec.cleanEnvironment) {
            // Reading environ is safe as long as nothing calls setenv concurrently; FrostIDE never does.
            for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                envStorage.emplace_back(*entry);
            }
        }
        for (const auto& [name, value] : spec.environment) {
            const std::string prefix = name + "=";
            std::erase_if(envStorage, [&](const std::string& entry) { return entry.starts_with(prefix); });
            if (value) {
                envStorage.push_back(prefix + *value);
            }
        }
        std::vector<char*> envp;
        for (std::string& entry : envStorage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        // TODO(M1): log the executable, the working directory and the arguments (PLAN.md 3.5).
        pid_t pid = 0;
        // posix_spawn, never posix_spawnp: the path is absolute and PATH is not searched.
        rc = posix_spawn(&pid, spec.executable.c_str(), &actions.actions, &attributes.attributes, argv.data(),
                         envp.data());
        if (rc != 0) {
            // The chdir file action fails with ENOENT or ENOTDIR, indistinguishable from the
            // executable's own errors, so the directory is checked to say which one failed
            // (with the same codes as the Windows launcher).
            struct stat info{};
            if ((rc == ENOENT || rc == ENOTDIR) && !spec.workingDirectory.empty()) {
                if (::stat(spec.workingDirectory.c_str(), &info) != 0) {
                    return fail(ErrorCode::NotFound, std::format("launch '{}': working directory '{}' does not exist",
                                                                 spec.executable, spec.workingDirectory));
                }
                if (!S_ISDIR(info.st_mode)) {
                    return fail(ErrorCode::InvalidArgument,
                                std::format("launch '{}': working directory '{}' is not a directory", spec.executable,
                                            spec.workingDirectory));
                }
            }
            return std::unexpected(errnoError(rc, "launch", spec.executable));
        }
        // The child ends close when this function returns, so EOF arrives as soon as the child
        // and its descendants close theirs.
        return std::make_unique<PosixProcess>(pid, spec.killOnClose, std::move(in.parent), std::move(out.parent),
                                              std::move(err.parent));
    }
};

// constinit and trivially destructible: no code runs before main or after exit.
constinit PosixProcessLauncher g_realLauncher;

} // namespace

IProcessLauncher& realProcessLauncher() noexcept { return g_realLauncher; }

} // namespace frost::io
