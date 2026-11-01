// SPDX-License-Identifier: Apache-2.0
//
// ICrashReporter: frost's crash reporting seam, and the crashpad implementation behind it
// (ADR 0022, D-D and D-G). One out-of-process crashpad_handler per frost process writes
// minidumps to <cache>/crashes/crashpad; nothing is ever uploaded.
//
/*
 * Handler start (start()):
 *   Windows: crashpad_handler.exe is started through io::IProcessLauncher (never
 *     CrashpadClient::StartHandler) with --database=\\?\<db>, --pipe-name=
 *     \\.\pipe\frost-crashpad-<pid>-<32 random hex digits>, --no-periodic-tasks and the
 *     prod/ver/plat annotations; ImagePathForm::Extended, killOnClose (the job ends the handler
 *     with this process), null standard streams and a clean environment that keeps SystemRoot,
 *     SystemDrive, windir, TEMP, TMP and the sanitizer options. start() then waits for the pipe
 *     (WaitNamedPipeW, 1 ms backing off to 50 ms, at most CrashReporterConfig::readyTimeout,
 *     failing early if the handler exits) and registers with SetHandlerIPCPipe, which installs
 *     crashpad's unhandled-exception filter.
 *   Linux: a socketpair (AF_UNIX, SOCK_SEQPACKET, SO_PASSCRED on both ends) whose one end
 *     becomes the handler's fd 3 (--initial-client-fd=3 --shared-client-connection,
 *     ProcessSpec::posixInheritedFds); this process registers the other
 *     end with SetHandlerSocket(end, handler pid), which installs crashpad's signal handlers and
 *     lets the handler ptrace this process. The handler exits when this process does.
 *   Never --url; crash reports are never forwarded to Windows Error Reporting or the system's
 *   crash reporter. An elevated process (TokenElevationTypeFull) starts no handler
 *   (Unsupported), so a dump never captures an administrator's memory into the user's cache.
 *
 * Lifetime: frost's program keeps its reporter, and the handler's IProcess inside it, for the
 * life of the process ([[clang::no_destroy]] statics), so the handler serves until the very end.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::diag {

// Why frost ends itself through crashNow().
enum class FatalReason : std::uint8_t {
    ContractViolation, // FROST_PRE/POST/ASSERT under enforce (frost/core/assert.hpp).
    Terminate,         // std::terminate (an uncaught exception, a noexcept violation, ...).
    Abort,             // abort() / SIGABRT.
    InvalidParameter,  // The MSVC CRT's invalid-parameter handler.
    PureCall,          // A pure virtual call.
    QtFatal,           // qFatal() or another fatal Qt message in the GUI.
    SelfTest,          // A deliberate crash requested by a test.
};

// "contract-violation", "terminate", "abort", "invalid-parameter", "purecall", "qt-fatal", "selftest".
[[nodiscard]] std::string_view toString(FatalReason reason) noexcept;

// The exception code crashNow() records on Windows ('FAT' in ASCII), and the exit status of a
// process whose crashNow() found no active reporter.
inline constexpr std::uint32_t kFatalExceptionCode = 0xE0464154u;

// The client-side annotations crashNow() and the program may set (crashpad StringAnnotations).
enum class AnnotationKey : std::uint8_t {
    ImagePath,     // frost.image_path: this process's image path.
    RestartedFrom, // frost.restarted_from: the launcher that restarted frost through its short link.
    SelfTest,      // frost.selftest: the self-test that crashes on purpose ("crash").
    Reason,        // frost.fatal_reason: toString(FatalReason), set by crashNow().
    Message,       // frost.message: crashNow()'s message (truncated to 1 KiB).
    Frames,        // frost.frames: return addresses at crashNow() ("0x..." separated by spaces).
};

struct CrashReporterConfig {
    std::string handlerPath;  // Absolute path of crashpad_handler(.exe).
    std::string databasePath; // <cache>/crashes/crashpad; must exist.
    std::string version;      // The ver= annotation (frost::build_info::kVersion).
    std::chrono::milliseconds readyTimeout{5000};
    // Windows: the handler's desktop (ProcessSpec::desktop); tests use a private one.
    std::string desktop{};
};

// What status() reports.
struct ReporterStatus {
    bool active = false;
    io::ProcessId handlerPid = 0;
    std::string handlerPath;
    std::string databasePath;
    std::string pipeName; // Windows only.
    IClock::Duration startDuration{};
};

class ICrashReporter {
public:
    virtual ~ICrashReporter() = default;

    /*
     * Starts the handler and registers this process with it (see the header comment). Called
     * once, from any thread. Errors: NotFound (no handler at the path), PermissionDenied (the
     * launch was refused), Timeout (the handler did not become ready in time; it is
     * terminated), IoError (it exited early or registration failed), Unsupported (an elevated
     * process, or a platform without crashpad).
     */
    [[nodiscard]] virtual Result<void> start(const CrashReporterConfig& config) = 0;

    // Whether start() succeeded. Thread-safe; async-signal-safe.
    [[nodiscard]] virtual bool active() const noexcept = 0;

    [[nodiscard]] virtual ReporterStatus status() const = 0;

    // Sets a client-side annotation, included in every later report. Thread-safe enough for
    // start-up use (one writer per key); values are truncated to the annotation's size.
    virtual void annotate(AnnotationKey key, std::string_view value) noexcept = 0;

    // Writes a report of the calling thread without ending the process. Errors: Unsupported
    // (not active, or no dump-without-crash on this platform).
    [[nodiscard]] virtual Result<void> dumpWithoutCrash(std::string_view message) = 0;

    /*
     * Ends the process with a crash report: sets frost.reason, frost.message and frost.frames,
     * then on Windows captures the context (RtlCaptureContext) and calls DumpAndCrash with
     * kFatalExceptionCode; on Linux raises SIGABRT, which crashpad's handler reports. Without an
     * active reporter it writes "frost: fatal <reason>: <message>" to stderr (WriteFile /
     * write(2)) and ends the process: Windows TerminateProcess(kFatalExceptionCode); Linux
     * PR_SET_DUMPABLE 0, SIGABRT's default action, raise(SIGABRT). Re-entrant calls (a fault
     * while reporting) end the process at once; a second thread waits for the first. Allocates
     * nothing; safe in a signal handler once active() is true.
     */
    [[noreturn]] virtual void crashNow(FatalReason reason, std::string_view message) noexcept = 0;
};

/*
 * The crashpad reporter. Readiness and registration go through `connector` (the real one by
 * default), so the start-up logic is testable with io::ScriptedProcessLauncher.
 */
class ICrashpadConnector {
public:
    // Windows: whether the handler's pipe exists and has a free instance (WaitNamedPipeW).
    [[nodiscard]] virtual bool pipeReady(const std::wstring& pipeName) noexcept = 0;
    // Windows: CrashpadClient::SetHandlerIPCPipe. Linux: CrashpadClient::SetHandlerSocket.
    [[nodiscard]] virtual bool registerPipe(const std::wstring& pipeName) noexcept = 0;
    [[nodiscard]] virtual bool registerSocket(int socket, io::ProcessId handlerPid) noexcept = 0;
    // Waits `delay` between readiness probes (a test double advances its ManualClock instead).
    virtual void sleepFor(IClock::Duration delay) noexcept = 0;
    // Whether this process runs with a full administrator token (Windows TokenElevation; always
    // false on Linux). start() refuses to start a handler then.
    [[nodiscard]] virtual bool processElevated() noexcept = 0;

protected:
    ~ICrashpadConnector() = default;
};

// The real connector (crashpad's CrashpadClient).
[[nodiscard]] ICrashpadConnector& realCrashpadConnector() noexcept;

// The handler's command line for `config` (without the executable), Windows pipe name
// `pipeName`. Exposed for the argument golden tests. Pure.
[[nodiscard]] std::vector<std::string> crashpadHandlerArguments(const CrashReporterConfig& config,
                                                                std::string_view pipeName, bool windowsRules);

// The handler's environment: the variables of `parent` that the handler keeps (see the header
// comment), in a fixed order. `parent` maps a name to its value. Pure.
[[nodiscard]] std::vector<std::pair<std::string, std::optional<std::string>>>
crashpadHandlerEnvironment(const std::function<std::optional<std::string>(std::string_view)>& parent,
                           bool windowsRules);

// A crashpad reporter. `launcher`, `clock` and `connector` must outlive it.
[[nodiscard]] std::unique_ptr<ICrashReporter>
createCrashpadReporter(io::IProcessLauncher& launcher, const IClock& clock, ICrashpadConnector& connector);
[[nodiscard]] std::unique_ptr<ICrashReporter> createCrashpadReporter(io::IProcessLauncher& launcher,
                                                                     const IClock& clock);

// The exit status of InMemoryCrashReporter::crashNow().
inline constexpr int kInMemoryFatalExit = 86;

/*
 * A test double: records annotations, dumps and the fatal call. start() succeeds unless
 * failWith is set; crashNow() records the call, writes it to stderr and calls
 * std::_Exit(kInMemoryFatalExit), so a death test can observe it.
 */
class InMemoryCrashReporter final : public ICrashReporter {
public:
    std::optional<ErrorCode> failWith;

    Result<void> start(const CrashReporterConfig& config) override;
    [[nodiscard]] bool active() const noexcept override { return m_active.load(std::memory_order_acquire); }
    [[nodiscard]] ReporterStatus status() const override;
    void annotate(AnnotationKey key, std::string_view value) noexcept override;
    Result<void> dumpWithoutCrash(std::string_view message) override;
    [[noreturn]] void crashNow(FatalReason reason, std::string_view message) noexcept override;

    [[nodiscard]] std::optional<std::string> annotation(AnnotationKey key) const;
    [[nodiscard]] std::vector<std::string> dumps() const;

private:
    mutable std::mutex m_mutex;
    std::atomic<bool> m_active{false};
    CrashReporterConfig m_config;
    std::vector<std::pair<AnnotationKey, std::string>> m_annotations;
    std::vector<std::string> m_dumps;
};

} // namespace frost::diag
