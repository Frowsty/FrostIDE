// SPDX-License-Identifier: Apache-2.0
//
// GitRunner (M5, ADR 0048): runs one git command through IProcessLauncher with the environment
// FrostIDE parses (English output, no terminal prompts, no editor), a time limit, cancellation of
// the whole process tree, and the output collected and optionally streamed line by line.
/*
 * Command line: <git> --no-pager -c core.quotepath=false -c color.ui=false [restricted options]
 * <arguments...>. Restricted mode (GitRunnerOptions::trusted false; PLAN.md section 3.5 "read-only
 * git") adds -c core.fsmonitor=false -c core.hooksPath=<noHooksDirectory>, so a repository the
 * user has not trusted cannot run its fsmonitor hook or hooks through us.
 *
 * Environment (the parent's, changed as follows):
 *   - LC_ALL=C and LANG=C, LANGUAGE removed: git's messages are English wherever they are parsed
 *     (checkout refusals, push rejections); paths are bytes either way (-z, core.quotepath=false).
 *   - GIT_TERMINAL_PROMPT=0: git fails at once instead of waiting for a terminal prompt that can
 *     never be answered; gitFailure() turns that into a clear message. Credentials come from git's
 *     own credential helper (Git Credential Manager on Windows). TODO(M5): frost-askpass.
 *   - GIT_EDITOR=:, GIT_SEQUENCE_EDITOR=: and GIT_MERGE_AUTOEDIT=no: git never opens an editor
 *     (":" is git's own "no editor" value).
 *   - GIT_OPTIONAL_LOCKS=0 for background commands (GitInvocation::background), so a status
 *     refresh never takes index.lock away from the user's own git in a terminal.
 *
 * Process tree. The child runs contained (killOnClose): a stop request or the time limit ends the
 * whole tree, and once git exits, anything it left running in the container (a credential helper
 * that inherited the pipes) is ended too, so the output always reaches its end.
 *
 * Threads: run() blocks the calling thread and uses two helper threads for the pipes (one more
 * when stdin data is given). GitRunner is immutable after construction; run() is thread-safe.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::vcs {

// Stdout and Stderr are git's; Command is the command line GitRepository echoes before it runs one
// ("git fetch --all --prune"), for the console.
enum class GitStream : std::uint8_t { Stdout, Stderr, Command };

// Receives each output line (UTF-8 bytes as git wrote them, without '\n' or '\r'; '\r' also ends a
// line, so progress updates arrive one by one). Called on the runner's pipe threads, one at a time.
using GitLineSink = std::function<void(GitStream stream, std::string_view line)>;

// The default limits: local commands, commands that run hooks (commit), remote commands.
inline constexpr std::chrono::seconds kLocalTimeout{60};
inline constexpr std::chrono::seconds kHookTimeout{300};
inline constexpr std::chrono::seconds kRemoteTimeout{600};

struct GitInvocation {
    std::vector<std::string> arguments; // After the global options ("status", "--porcelain=v2", ...).
    std::string workingDirectory;       // Absolute; the repository's work tree, usually.
    std::string stdinData;              // Written to git's stdin, which is then closed; "" gives the null device.
    IClock::Duration timeout = kLocalTimeout;
    std::stop_token stop;       // A stop ends the process tree; run() fails with Cancelled.
    bool background = false;    // GIT_OPTIONAL_LOCKS=0 (header comment).
    bool requiresTrust = false; // Recheck trust immediately before launching a mutating command.
    GitLineSink onLine;         // Optional streaming of both streams.
    // Extra environment changes, applied after the runner's own (a value sets, nullopt removes).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
};

// What one git command produced. A non-zero exit is not an error of run(): callers decide.
struct GitOutput {
    int exitCode = 0;
    std::string stdoutBytes;
    std::string stderrBytes;

    [[nodiscard]] bool succeeded() const noexcept { return exitCode == 0; }
};

struct GitRunnerOptions {
    // false: restricted mode (header comment).
    bool trusted = true;
    // Optional live trust check, safe to call from the Git workers.
    std::function<bool()> stillTrusted;
    // core.hooksPath in restricted mode: an absolute directory FrostIDE controls that holds no hooks
    // (it need not exist). "" uses a fixed path below the system's temporary directory.
    std::string noHooksDirectory;
    // Environment changes for every command, after the runner's own and before the invocation's
    // (tests isolate git from the machine's configuration with GIT_CONFIG_GLOBAL, GIT_CONFIG_NOSYSTEM).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
};

class GitRunner {
public:
    // `launcher` must outlive the runner; `gitPath` is absolute (findGit's result).
    GitRunner(io::IProcessLauncher& launcher, std::string gitPath, GitRunnerOptions options = {});

    [[nodiscard]] const std::string& gitPath() const noexcept { return m_gitPath; }
    [[nodiscard]] bool trusted() const {
        return m_options.trusted && (!m_options.stillTrusted || m_options.stillTrusted());
    }
    [[nodiscard]] io::IProcessLauncher& launcher() const noexcept { return *m_launcher; }

    // The process spec of `invocation` (header comment). Pure; no I/O.
    [[nodiscard]] io::ProcessSpec processSpec(const GitInvocation& invocation) const;

    /*
     * Runs git and collects its output. Errors: the launcher's (NotFound when git is gone,
     * InvalidArgument for a relative working directory), Timeout (the limit passed; the tree was
     * ended), Cancelled (the stop token; the tree was ended), IoError.
     */
    [[nodiscard]] Result<GitOutput> run(const GitInvocation& invocation) const;

    // `git --version`'s output, trimmed ("git version 2.56.0.windows.1"). Errors: those of run(),
    // IoError when git exits with a failure.
    [[nodiscard]] Result<std::string> version() const;

private:
    io::IProcessLauncher* m_launcher;
    std::string m_gitPath;
    GitRunnerOptions m_options;
};

/*
 * The error for a git command that failed: "<what> failed: <git's last message lines>", with a
 * plain-language explanation first for the failures FrostIDE recognises (credentials needed while
 * terminal prompts are off, a rejected push, an unreachable remote, no upstream, a lock file left
 * by another git). Code IoError, or Busy for a lock file. Pure.
 */
[[nodiscard]] Error gitFailure(std::string_view what, const GitOutput& output);

// `text` without leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trimGitText(std::string_view text) noexcept;

} // namespace frost::vcs
