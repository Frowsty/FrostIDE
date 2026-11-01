// SPDX-License-Identifier: Apache-2.0
//
// IProcessLauncher: the only way FrostIDE starts child processes (CLAUDE.md,
// PLAN.md sections 3.5 and 6.5), plus the process handle it returns and the
// runToCompletion convenience.
//
/*
 * Launch rules (PLAN.md section 3.5, trust model):
 *   - The executable is an absolute path. Relative names fail with
 *     InvalidArgument; the launcher never searches PATH, the current directory
 *     or the project directory. Callers resolve tools from detection results.
 *   - Arguments are UTF-8 and reach the child exactly: on Windows they are
 *     quoted with the inverse of the MSVC CRT / CommandLineToArgvW parsing
 *     rules (appendWindowsArgument below); on POSIX they are passed as argv.
 *   - Windows batch files (.bat, .cmd) are refused with Unsupported: cmd.exe
 *     re-parses their command line with different rules, so argument quoting
 *     cannot be made safe here (the "BatBadBut" class of injection bugs).
 *     TODO(M2): a dedicated cmd.exe launcher with cmd-specific escaping for
 *     vcvarsall.bat and friends.
 *   - Only the child's three standard handles are inherited: Windows passes an
 *     explicit PROC_THREAD_ATTRIBUTE_HANDLE_LIST; POSIX opens every descriptor
 *     with O_CLOEXEC and the child closes everything above 2 before exec.
 *   - TODO(M1): log the resolved executable, working directory and arguments
 *     (PLAN.md 3.5 requires it) once frost::core has logging.
 *
 * Pipes. Streams configured as Stdio::Pipe are connected to the IProcess:
 * writeStdin/readStdout/readStderr block until done (the asio reactor that
 * makes them asynchronous arrives in M1). A caller that reads only one pipe
 * while the child fills the other deadlocks; communicate() and
 * runToCompletion() drain all pipes together and are the safe default.
 * TODO(M1): async pipe I/O on the reactor (IOCP / epoll).
 *
 * Process tree. With killOnClose (the default) the child is contained: on
 * Windows it starts inside a new Job Object with
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; on POSIX it leads a new session, and so a
 * new process group, without a controlling terminal (opening /dev/tty fails
 * with ENXIO, so terminal prompts fail fast instead of stopping the child with
 * SIGTTIN; an inherited terminal stdin becomes the null device, see
 * Stdio::Inherit). terminate() then ends every process still in the container,
 * and destroying the IProcess ends whatever is still running in it. On Windows
 * that is the whole tree (descendants cannot break away from the job). On POSIX
 * it is the child's process group: a descendant that starts its own session or
 * group (setsid/setpgid, as daemons do) leaves it and is not ended. Without
 * killOnClose only the child itself is terminated, destroying the IProcess
 * leaves it running, and on POSIX it shares this process's session and terminal.
 * TODO(M3): supervision with restart and backoff (PLAN.md section 3.1; the M3 supervisor).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {

// How one of the child's standard streams is connected.
enum class Stdio : unsigned char {
    // The parent's handle, or the null device if the parent has none that a child could
    // inherit (a GUI process; on POSIX a closed descriptor, or one marked close-on-exec,
    // such as a descriptor frost::io itself opened in a slot the process had closed).
    // POSIX with killOnClose: a terminal stdin is replaced by the null device too. The
    // contained child's session is outside the terminal's job control, so it could read
    // the terminal while the shell is in the foreground (after `frost &`, say) and take
    // keystrokes meant for it. Terminal stdout and stderr are kept.
    Inherit,
    Null,            // The null device: reads see EOF, writes are discarded.
    Pipe,            // A pipe to the IProcess.
    MergeIntoStdout, // stderr only: the same handle as the child's stdout (like 2>&1).
};

/*
 * How the Windows launcher passes the executable to CreateProcessW. The child sees the
 * same form as its own image path (GetModuleFileNameW), and the loader resolves the DLLs
 * beside it in that form. Ignored on POSIX. The working directory always follows Auto's rule.
 */
enum class ImagePathForm : unsigned char {
    Auto,     // Plain below MAX_PATH (260 UTF-16 units), \\?\ from there on.
    Plain,    // Never prefixed (a verbatim \\?\ executable is passed as given). CreateProcessW's own
              // error comes back if it refuses the length; from MAX_PATH on it does, unless the
              // parent's long paths are effective (LongPathsEnabled=1 and a longPathAware manifest).
    Extended, // Always \\?\ (\\?\UNC\ for UNC paths).
};

struct ProcessSpec {
    std::string executable{};             // Absolute path (UTF-8).
    std::vector<std::string> arguments{}; // argv[1..]; argv[0] is always the executable path.
    std::string workingDirectory{};       // Absolute path, or empty to use the parent's current directory.

    // Start from an empty environment instead of a copy of the parent's.
    bool cleanEnvironment = false;
    // Applied in order on top of the parent's (or the empty) environment: a value sets the
    // variable, nullopt removes it. Names are case-insensitive on Windows, as the OS treats them.
    // TODO(M2): an environment policy per launch class. frost's start-up sets
    // NoDefaultCurrentDirectoryInExePath=1 (frost/app/startup.hpp) and every child inherits
    // it, so cmd.exe in user build scripts (MSBuild Exec tasks, pre/post-build events, CMake
    // configure) no longer runs a tool from its current directory by bare name. The M2 trust
    // ADR records whether trusted user-script children get it stripped (Visual Studio's
    // behaviour) or keep it; restricted children (read-only git, clangd, bundled tools) keep it.
    std::vector<std::pair<std::string, std::optional<std::string>>> environment{};

    Stdio stdinMode = Stdio::Null;
    Stdio stdoutMode = Stdio::Pipe;
    Stdio stderrMode = Stdio::Pipe;

    // Contain the child and its descendants: a Job Object on Windows, a new session and process group
    // on POSIX (see the header comment for what each covers).
    bool killOnClose = true;

    // Windows: see ImagePathForm. Ignored on POSIX.
    ImagePathForm imagePathForm = ImagePathForm::Auto;
    /*
     * Windows: STARTUPINFOW::lpDesktop, "<window station>\<desktop>" (UTF-8, no NUL). Empty
     * inherits the parent's desktop. Tests use a private desktop that is never shown, so a
     * window or message box the child draws itself never reaches the user's screen. The
     * system's hard-error boxes (the loader's, Windows Error Reporting's) are not the child's
     * windows; the error mode the child inherits governs those (tests/support/private_desktop.hpp).
     * The desktop must exist and admit the child's user. A missing one is not a launch
     * error: CreateProcessW succeeds, and a child that imports user32 then ends with
     * 0xC0000142 (STATUS_DLL_INIT_FAILED) before its own code runs, raising a hard error on
     * the way (measured 2026-11-01 with io_test_child; pinned by
     * RealProcessLauncher.MissingDesktopEndsTheChildBeforeItRuns). Ignored on POSIX.
     */
    std::string desktop{};
};

// The exit code reported after terminate(): 128 + SIGKILL, the POSIX shell's
// convention, so Windows and POSIX agree.
inline constexpr int kTerminatedExitCode = 137;

struct ExitStatus {
    // Windows: the process exit code (DWORD bits as int, so NTSTATUS crashes are negative).
    // POSIX: the exit status, or 128 + the signal number if a signal ended the process.
    int exitCode = 0;
    // POSIX: the signal that ended the process, 0 if it exited. Always 0 on Windows.
    int signal = 0;

    friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

// What communicate() and runToCompletion() return.
struct ProcessResult {
    int exitCode = 0; // As ExitStatus::exitCode.
    int signal = 0;   // As ExitStatus::signal.
    std::string stdoutBytes{};
    std::string stderrBytes{};
};

using ProcessId = std::uint32_t;

/*
 * A running (or finished) child process. Owned through std::unique_ptr, so,
 * unlike the reference-passed seams, it has a public virtual destructor.
 * Destruction closes the pipes and, with killOnClose, ends every process still
 * in the child's container (see the header comment).
 *
 * Thread safety: operations on different streams (stdin, stdout, stderr) and
 * wait()/terminate() may run concurrently from different threads; operations
 * on the same stream must not. communicate() must not overlap any other call.
 */
class IProcess {
public:
    virtual ~IProcess() = default;
    IProcess(const IProcess&) = delete ("a process has exactly one owner");
    IProcess& operator=(const IProcess&) = delete ("a process has exactly one owner");

    [[nodiscard]] virtual ProcessId pid() const noexcept = 0;

    // Writes all of `bytes` to the child's stdin, blocking while the pipe is full.
    // Errors: InvalidArgument (stdin is not a pipe or was closed), IoError (the child closed it).
    [[nodiscard]] virtual Result<void> writeStdin(std::string_view bytes) = 0;
    // Closes the parent's end of the stdin pipe, so the child sees EOF. Idempotent.
    virtual void closeStdin() noexcept = 0;

    // Reads up to buffer.size() bytes, blocking until at least one byte or EOF;
    // returns 0 at EOF. Errors: InvalidArgument (not a pipe), IoError.
    [[nodiscard]] virtual Result<std::size_t> readStdout(std::span<char> buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> readStderr(std::span<char> buffer) = 0;

    // Waits for the process to exit (forever when `timeout` is nullopt) and returns its
    // status; later calls return the same status. Errors: Timeout (still running), IoError.
    [[nodiscard]] virtual Result<ExitStatus> wait(std::optional<IClock::Duration> timeout = std::nullopt) = 0;

    // Forcefully ends the process (with killOnClose, every process in its container) with
    // kTerminatedExitCode or SIGKILL. Succeeds if it has already exited. Does not wait; call wait() afterwards.
    [[nodiscard]] virtual Result<void> terminate() = 0;

    /*
     * Writes `stdinData` to the stdin pipe and closes it, reads stdout and stderr
     * until EOF, and waits for exit, all concurrently and bounded by `timeout` from
     * the call. Streams that are not pipes contribute no bytes. If the child closes
     * stdin early the rest of `stdinData` is dropped (not an error). When the deadline
     * passes the process is terminated as by terminate(), the call waits up to 5 s
     * for the child to exit (it is reaped only when the IProcess is destroyed), and
     * fails with Timeout; the partial output is discarded.
     * Errors: InvalidArgument (stdinData given but stdin is not a pipe), Timeout, IoError.
     */
    [[nodiscard]] virtual Result<ProcessResult> communicate(std::string_view stdinData, IClock::Duration timeout) = 0;

protected:
    IProcess() = default;
};

class IProcessLauncher {
public:
    /*
     * Starts a process. Errors: InvalidArgument (a spec validateProcessSpec rejects; a
     * working directory that is not a directory or, on Windows, too long to be a current
     * directory; a file that is not an executable image), Unsupported (batch files on
     * Windows), NotFound (no such executable or working directory), PermissionDenied (not
     * executable), Busy (the executable is open for writing, or the system is out of
     * processes), IoError. Thread-safe.
     */
    [[nodiscard]] virtual Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) = 0;

protected:
    constexpr IProcessLauncher() noexcept = default;
    constexpr IProcessLauncher(const IProcessLauncher&) noexcept = default;
    constexpr IProcessLauncher& operator=(const IProcessLauncher&) noexcept = default;
    ~IProcessLauncher() = default;
};

// The process-wide real launcher (CreateProcessW on Windows, posix_spawn on Linux).
// Stateless and thread-safe.
[[nodiscard]] IProcessLauncher& realProcessLauncher() noexcept;

// launch() followed by communicate(): the safe way to run a tool and collect its
// output. Both pipes are drained concurrently, so arbitrarily large output works.
// Errors: those of launch() and communicate(), with the executable as context.
[[nodiscard]] Result<ProcessResult> runToCompletion(IProcessLauncher& launcher, const ProcessSpec& spec,
                                                    std::string_view stdinData, IClock::Duration timeout);

// Whether this platform applies the Windows path and batch-file rules in validateProcessSpec.
#ifdef _WIN32
inline constexpr bool kWindowsProcessRules = true;
#else
inline constexpr bool kWindowsProcessRules = false;
#endif

/*
 * The checks every launcher (real and scripted) applies before starting anything:
 * an absolute executable path that normalizePath accepts; a working directory
 * that is empty or likewise absolute and valid; NUL-free arguments; environment
 * names that are non-empty and free of '=' and NUL, values free of NUL;
 * MergeIntoStdout only for stderr. With `windowsRules` also: Windows path syntax;
 * no .bat/.cmd executable (Unsupported), judged on both the given and the
 * normalised spelling, since "dir\t.bat\." starts dir\t.bat; no quote in the
 * executable; a command line under CreateProcessW's 32,767-unit limit; and
 * well-formed UTF-8 in the command line, the working directory, the
 * environment names and values and the desktop name, which must also be
 * NUL-free (the launcher converts them to UTF-16).
 * Errors: InvalidArgument, Unsupported.
 */
[[nodiscard]] Result<void> validateProcessSpec(const ProcessSpec& spec, bool windowsRules = kWindowsProcessRules);

/*
 * Appends `argument` to a Windows command line so that the MSVC CRT's argv
 * parser and CommandLineToArgvW both yield it unchanged. This is the exact
 * inverse of the rules in Microsoft's "Parsing C++ command-line arguments"
 * (https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args,
 * section "Parsing C++ command-line arguments") and the CommandLineToArgvW
 * reference (https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw):
 * arguments are separated by spaces or tabs; a double-quoted span may contain
 * them; backslashes are literal unless they precede a double quote, where
 * 2n backslashes + quote yield n backslashes and a delimiter, and 2n+1 yield
 * n backslashes and a literal quote. So: arguments that are non-empty and
 * contain no space, tab, newline, vertical tab or quote are appended as is;
 * all others are wrapped in quotes, each quote is preceded by twice the
 * backslashes before it plus one, and backslashes before the closing quote are
 * doubled. Does not add a separating space. Pure and portable (UTF-8 in and
 * out), so it is unit-tested on every platform.
 */
void appendWindowsArgument(std::string& commandLine, std::string_view argument);

// The full Windows command line for `executable` and `arguments`: the executable
// quoted as argv[0] (whose parsing rules differ: no escapes, so it must not contain
// a quote) followed by each argument. Errors: InvalidArgument (quote in the executable).
[[nodiscard]] Result<std::string> buildWindowsCommandLine(std::string_view executable,
                                                          const std::vector<std::string>& arguments);

} // namespace frost::io
