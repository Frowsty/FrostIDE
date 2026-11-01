// SPDX-License-Identifier: Apache-2.0
//
// The Linux half of the crash reporter (crash_internal.hpp): the handler's socketpair, its
// launch with the socket as fd 3, registration through crashpad's CrashpadClient
// (Apache-2.0), and the fatal-path primitives.

#include "frost/io/os_error.hpp"

#include "crash_internal.hpp"
#include "internal.hpp"
#include <client/crashpad_client.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <util/file/file_io.h>
#include <util/misc/capture_context.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <signal.h>
#include <unistd.h>
#include <unwind.h>

namespace frost::diag {
namespace {

crashpad::CrashpadClient& crashpadClient() noexcept {
    // Registered once and needed until the process ends, so never destroyed.
    [[clang::no_destroy]] static crashpad::CrashpadClient client;
    return client;
}

class PosixCrashpadConnector final : public ICrashpadConnector {
public:
    constexpr PosixCrashpadConnector() noexcept = default;

    bool pipeReady(const std::wstring&) noexcept override { return false; }
    bool registerPipe(const std::wstring&) noexcept override { return false; }

    bool registerSocket(int socket, io::ProcessId handlerPid) noexcept override {
        try {
            // Installs crashpad's signal handlers (chaining to the ones installed before) and
            // makes the handler this process's ptracer (PR_SET_PTRACER).
            return crashpadClient().SetHandlerSocket(crashpad::ScopedFileHandle(socket),
                                                     static_cast<pid_t>(handlerPid));
        } catch (...) {
            return false;
        }
    }

    void sleepFor(IClock::Duration delay) noexcept override { std::this_thread::sleep_for(delay); }

    bool processElevated() noexcept override { return false; }
};

constinit PosixCrashpadConnector g_connector;

class Descriptor {
public:
    explicit Descriptor(int fd) noexcept : m_fd(fd) {}
    Descriptor(const Descriptor&) = delete ("owns a descriptor");
    Descriptor& operator=(const Descriptor&) = delete ("owns a descriptor");
    ~Descriptor() { reset(); }
    [[nodiscard]] int get() const noexcept { return m_fd; }
    int release() noexcept { return std::exchange(m_fd, -1); }
    void reset() noexcept {
        if (m_fd >= 0) {
            (void)::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd;
};

std::optional<std::string> parentVariable(std::string_view name) {
    std::string value = environmentValue(name);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

ICrashpadConnector& realCrashpadConnector() noexcept { return g_connector; }

namespace detail {

Result<HandlerConnection> launchAndRegister(io::IProcessLauncher& launcher, [[maybe_unused]] const IClock& clock,
                                            ICrashpadConnector& connector, const CrashReporterConfig& config) {
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
        return std::unexpected(io::errnoError(errno, "socketpair for the crash handler"));
    }
    Descriptor mine(pair[0]);
    Descriptor theirs(pair[1]);
    /* The kernel attaches the sender's credentials to a message only if the receiving end has
       SO_PASSCRED when the message is sent. The handler sets it on its end at start-up, but a
       crash in the first milliseconds (before the handler ran that far) would then reach it
       without credentials and be refused ("invalid credentials", measured 2026-11-01), so the
       option is set here on both ends, before the handler exists, as crashpad's
       UnixCredentialSocket::CreateCredentialSocketpair does (Apache-2.0). */
    const int passCredentials = 1;
    for (const int end : {mine.get(), theirs.get()}) {
        if (::setsockopt(end, SOL_SOCKET, SO_PASSCRED, &passCredentials, sizeof passCredentials) != 0) {
            return std::unexpected(io::errnoError(errno, "SO_PASSCRED on the crash handler's socket"));
        }
    }

    io::ProcessSpec spec;
    spec.executable = config.handlerPath;
    spec.arguments = crashpadHandlerArguments(config, {}, false);
    spec.cleanEnvironment = true;
    spec.environment = crashpadHandlerEnvironment(&parentVariable, false);
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Null;
    spec.stderrMode = io::Stdio::Null;
    spec.killOnClose = true;
    spec.posixInheritedFds = {theirs.get()};
    auto process = launcher.launch(spec);
    theirs.reset(); // The handler owns its end now.
    if (!process) {
        return std::unexpected(std::move(process.error()).withContext("starting the crash handler"));
    }
    // The handler serves the socket from its first instruction (--initial-client-fd), so there
    // is no readiness to wait for; a handler that exits at once is still caught here.
    if (auto exited = (*process)->wait(IClock::Duration::zero()); exited) {
        return fail(ErrorCode::IoError, std::format("the crash handler exited with {} before registration",
                                                    exited->signal != 0 ? -exited->signal : exited->exitCode));
    }
    if (!connector.registerSocket(mine.release(), (*process)->pid())) {
        (void)(*process)->terminate();
        (void)(*process)->wait(std::chrono::seconds(1));
        return fail(ErrorCode::IoError, "registration with the crash handler failed");
    }
    return HandlerConnection{std::move(*process), {}};
}

bool processIsElevated() noexcept { return false; }

namespace {

struct UnwindState {
    std::uintptr_t* frames;
    std::size_t capacity;
    std::size_t skip;
    std::size_t count;
};

_Unwind_Reason_Code collectFrame(_Unwind_Context* context, void* argument) noexcept {
    auto& state = *static_cast<UnwindState*>(argument);
    const auto ip = static_cast<std::uintptr_t>(_Unwind_GetIP(context));
    if (ip == 0) {
        return _URC_END_OF_STACK;
    }
    if (state.skip > 0) {
        --state.skip;
        return _URC_NO_REASON;
    }
    state.frames[state.count++] = ip;
    return state.count == state.capacity ? _URC_END_OF_STACK : _URC_NO_REASON;
}

} // namespace

std::size_t captureFrames(std::uintptr_t* frames, std::size_t capacity, std::size_t skip) noexcept {
    /* The Itanium C++ ABI's _Unwind_Backtrace (libgcc_s), into the caller's buffer without
       allocating; each entry is _Unwind_GetIP, a return address like Windows'
       RtlCaptureStackBackTrace gives. cpptrace's safe_generate_raw_trace is no option: its
       _Unwind back-end returns no frames (can_signal_safe_unwind() is false; measured
       2026-11-01), and its libunwind back-end would add a dependency. libgcc 14 finds each
       frame's unwind table through glibc's _dl_find_object (libgcc_s imports
       _dl_find_object@GLIBC_2.35 on Ubuntu 24.04, glibc 2.39), which takes no lock and does
       not allocate (ADR 0022). +1 skips this function. */
    if (capacity == 0) {
        return 0;
    }
    UnwindState state{frames, capacity, skip + 1, 0};
    (void)_Unwind_Backtrace(&collectFrame, &state);
    return state.count;
}

void writeToStderr(std::string_view text) noexcept {
    while (!text.empty()) {
        const ssize_t written = ::write(STDERR_FILENO, text.data(), text.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}

void crashThroughHandler() noexcept {
    // crashpad's SIGABRT handler writes the report, then restores the previous action (the
    // last-chance handler) and raises again.
    sigset_t abortOnly;
    sigemptyset(&abortOnly);
    sigaddset(&abortOnly, SIGABRT);
    (void)::pthread_sigmask(SIG_UNBLOCK, &abortOnly, nullptr);
    (void)::raise(SIGABRT);
    endWithoutReport();
}

void dumpThroughHandler() noexcept {
    crashpad::NativeCPUContext context;
    crashpad::CaptureContext(&context);
    crashpad::CrashpadClient::DumpWithoutCrash(&context);
}

void endWithoutReport() noexcept {
    (void)::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    (void)::sigaction(SIGABRT, &defaultAction, nullptr);
    sigset_t abortOnly;
    sigemptyset(&abortOnly);
    sigaddset(&abortOnly, SIGABRT);
    (void)::pthread_sigmask(SIG_UNBLOCK, &abortOnly, nullptr);
    (void)::raise(SIGABRT);
    ::_exit(134);
}

} // namespace detail
} // namespace frost::diag
