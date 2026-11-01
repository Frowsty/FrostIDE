// SPDX-License-Identifier: Apache-2.0
//
// The Windows half of the crash reporter (crash_internal.hpp): the handler's pipe name, its
// launch and readiness, registration through crashpad's CrashpadClient (Apache-2.0), and the
// fatal-path primitives. Wide Win32 APIs only.

#include "frost/io/utf.hpp"

#include "crash_internal.hpp"
#include "internal.hpp"
#include <client/crashpad_client.h>

#include <algorithm>
#include <chrono>
#include <format>

#include <windows.h>

#include <bcrypt.h>

#if !defined(_M_X64)
#error "frost::diag's Windows crash path supports x64 only"
#endif

namespace frost::diag {
namespace {

crashpad::CrashpadClient& crashpadClient() noexcept {
    // Registered once and needed until the process ends, so never destroyed.
    [[clang::no_destroy]] static crashpad::CrashpadClient client;
    return client;
}

class WinCrashpadConnector final : public ICrashpadConnector {
public:
    constexpr WinCrashpadConnector() noexcept = default;

    bool pipeReady(const std::wstring& pipeName) noexcept override {
        // 1 ms, not 0: a zero timeout means the server's default (NMPWAIT_USE_DEFAULT_WAIT).
        // A pipe that does not exist yet fails at once with ERROR_FILE_NOT_FOUND.
        return WaitNamedPipeW(pipeName.c_str(), 1) != FALSE;
    }

    bool registerPipe(const std::wstring& pipeName) noexcept override {
        try {
            return crashpadClient().SetHandlerIPCPipe(pipeName);
        } catch (...) {
            return false;
        }
    }

    bool registerSocket(int, io::ProcessId) noexcept override { return false; }

    void sleepFor(IClock::Duration delay) noexcept override {
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();
        Sleep(static_cast<DWORD>(std::clamp<long long>(millis, 1, 1000)));
    }

    bool processElevated() noexcept override { return detail::processIsElevated(); }
};

constinit WinCrashpadConnector g_connector;

// "\\.\pipe\frost-crashpad-<pid>-<32 hex digits>", the digits from the system RNG, so another
// process can neither predict nor pre-create the pipe (its DACL admits only this user anyway).
Result<std::string> makePipeName() {
    unsigned char random[16];
    const NTSTATUS status =
        BCryptGenRandom(nullptr, random, static_cast<ULONG>(sizeof(random)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return fail(ErrorCode::IoError,
                    std::format("BCryptGenRandom failed with 0x{:08x}", static_cast<unsigned>(status)));
    }
    std::string name = std::format("\\\\.\\pipe\\frost-crashpad-{}-", GetCurrentProcessId());
    for (const unsigned char byte : random) {
        name += std::format("{:02x}", byte);
    }
    return name;
}

std::optional<std::string> parentVariable(std::string_view name) {
    std::string value = environmentValue(name);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

void stopHandler(io::IProcess& process) noexcept {
    (void)process.terminate();
    (void)process.wait(std::chrono::seconds(1));
}

} // namespace

ICrashpadConnector& realCrashpadConnector() noexcept { return g_connector; }

namespace detail {

Result<HandlerConnection> launchAndRegister(io::IProcessLauncher& launcher, const IClock& clock,
                                            ICrashpadConnector& connector, const CrashReporterConfig& config) {
    auto pipeName = makePipeName();
    if (!pipeName) {
        return std::unexpected(std::move(pipeName.error()));
    }
    auto widePipe = io::toWide(*pipeName);
    if (!widePipe) {
        return std::unexpected(std::move(widePipe.error()));
    }

    io::ProcessSpec spec;
    spec.executable = config.handlerPath;
    spec.arguments = crashpadHandlerArguments(config, *pipeName, true);
    spec.cleanEnvironment = true;
    spec.environment = crashpadHandlerEnvironment(&parentVariable, true);
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Null;
    spec.stderrMode = io::Stdio::Null;
    spec.killOnClose = true; // The job ends the handler when frost ends.
    spec.imagePathForm = io::ImagePathForm::Extended;
    spec.desktop = config.desktop;
    auto process = launcher.launch(spec);
    if (!process) {
        return std::unexpected(std::move(process.error()).withContext("starting the crash handler"));
    }

    // Readiness: the pipe exists once the handler listens. Poll with a backoff from 1 ms to
    // 50 ms, failing early if the handler has exited.
    const IClock::SteadyTime deadline = clock.steadyNow() + config.readyTimeout;
    IClock::Duration delay = std::chrono::milliseconds(1);
    while (!connector.pipeReady(*widePipe)) {
        auto exited = (*process)->wait(IClock::Duration::zero());
        if (exited) {
            return fail(ErrorCode::IoError, std::format("the crash handler exited with 0x{:08x} before it was ready",
                                                        static_cast<unsigned>(exited->exitCode)));
        }
        if (exited.error().code() != ErrorCode::Timeout) {
            stopHandler(**process);
            return std::unexpected(std::move(exited.error()).withContext("waiting for the crash handler"));
        }
        if (clock.steadyNow() >= deadline) {
            stopHandler(**process);
            return fail(ErrorCode::Timeout, std::format("the crash handler did not open its pipe within {} ms",
                                                        config.readyTimeout.count()));
        }
        connector.sleepFor(delay);
        delay = std::min<IClock::Duration>(delay * 2, std::chrono::milliseconds(50));
    }
    if (!connector.registerPipe(*widePipe)) {
        stopHandler(**process);
        return fail(ErrorCode::IoError, "registration with the crash handler failed");
    }
    return HandlerConnection{std::move(*process), std::move(*pipeName)};
}

std::size_t captureFrames(std::uintptr_t* frames, std::size_t capacity, std::size_t skip) noexcept {
    /* RtlCaptureStackBackTrace, not cpptrace: cpptrace's signal-safe tracer has no Windows
       back-end (safe_generate_raw_trace returns no frames there; measured 2026-11-01), and this
       call allocates nothing and takes no lock. +1 skips this function. */
    const auto wanted = static_cast<ULONG>(std::min<std::size_t>(capacity, 0xFFFF));
    return RtlCaptureStackBackTrace(static_cast<ULONG>(skip + 1), wanted, reinterpret_cast<void**>(frames), nullptr);
}

bool processIsElevated() noexcept {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    // TokenIsElevated covers TokenElevationTypeFull and an administrator without UAC.
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

void writeToStderr(std::string_view text) noexcept {
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    while (!text.empty()) {
        DWORD written = 0;
        if (WriteFile(handle, text.data(), static_cast<DWORD>(std::min<std::size_t>(text.size(), 65536)), &written,
                      nullptr) == FALSE ||
            written == 0) {
            return;
        }
        text.remove_prefix(written);
    }
}

void crashThroughHandler() noexcept {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = static_cast<DWORD>(kFatalExceptionCode);
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};
    // The handler writes the dump, then terminates this process with the exception code.
    crashpad::CrashpadClient::DumpAndCrash(&pointers);
    endWithoutReport();
}

void dumpThroughHandler() noexcept {
    CONTEXT context{};
    RtlCaptureContext(&context);
    crashpad::CrashpadClient::DumpWithoutCrash(context);
}

void endWithoutReport() noexcept {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kFatalExceptionCode));
    __builtin_trap();
}

} // namespace detail
} // namespace frost::diag
