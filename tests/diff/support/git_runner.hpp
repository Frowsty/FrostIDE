// SPDX-License-Identifier: Apache-2.0
//
// GitRunner: `git diff --histogram --no-index` as the independent oracle of the frost::diff
// differential tests (ADR 0019, D16), run through io::IProcessLauncher with an absolute git path,
// a pinned environment and pinned configuration, plus the dialect probe and `git apply`. No gtest.
/*
 * Isolation. Every run uses workingDirectory = a batch directory under the caller's scratch
 * directory and the environment GIT_CONFIG_NOSYSTEM=1, GIT_CONFIG_GLOBAL=<scratch>/empty.gitconfig,
 * HOME and XDG_CONFIG_HOME=<scratch>, GIT_CEILING_DIRECTORIES=<scratch's parent>,
 * GIT_ATTR_NOSYSTEM=1 (Git for Windows ships a system gitattributes with diff drivers),
 * GIT_TERMINAL_PROMPT=0 and LC_ALL=C, with GIT_DIR, GIT_WORK_TREE, GIT_EXTERNAL_DIFF,
 * GIT_DIFF_OPTS, GIT_CONFIG, GIT_CONFIG_PARAMETERS and GIT_CONFIG_COUNT removed. Every command
 * starts with -c core.autocrlf=false -c core.safecrlf=false -c core.quotepath=false
 * -c core.fsmonitor=false -c core.hooksPath=<scratch>/no-hooks -c diff.suppressBlankEmpty=false
 * -c diff.noprefix=false -c diff.mnemonicPrefix=false -c diff.relative=false.
 *
 * Success of a diff run needs all of: exit 0 or 1 with empty stderr (`git diff --no-index` exits 1
 * and prints "error: Could not access" for a missing path, so the exit code alone proves
 * nothing); exactly one "diff --git" section per case whose bytes differ; no section for a
 * byte-identical case; no unknown or duplicate names; well-formed @@ headers. Anything else is an
 * error quoting stderr. Each run has a 60 s timeout.
 *
 * Thread safety: one runner per thread (it numbers its batch directories).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include "diff/support/batch_writer.hpp"
#include "diff/support/goldens.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff::test {

// The two histogram dialects (ADR 0019, D6): git before the 2.54 hunk-shift fix, and after it.
enum class GitDialect : std::uint8_t { Classic, New };

[[nodiscard]] std::string_view dialectName(GitDialect dialect) noexcept;

// git's output for one case.
struct GitSection {
    bool present = false; // False for a byte-identical case (no section).
    HunkList hunks;       // One per @@ header; at -U0 exactly the hunks.
    std::string body;     // From the first @@ line to the section's end, the text after the second
                          // "@@" of each header stripped (funcname context, out of scope until M5).
};

class GitRunner {
public:
    // `git` is an absolute path; `scratch` an existing absolute directory the caller owns and
    // removes (a ScratchDir). The launcher must outlive the runner.
    GitRunner(io::IProcessLauncher& launcher, std::string git, std::string scratch);

    // Writes <scratch>/empty.gitconfig and creates <scratch>/no-hooks. Errors: IoError.
    [[nodiscard]] Result<void> prepare();

    // `git --version`, trimmed. Errors: as diff() (launch failures say "git not found").
    [[nodiscard]] Result<std::string> version();

    // One `git diff --no-index ... -U<context> a b` over the cases (written by writeBatch); one
    // section per case, in case order. Errors: InvalidArgument for any violation of the success
    // criteria (header comment), Timeout, and launch failures as NotFound "git not found: ...".
    [[nodiscard]] Result<std::vector<GitSection>> diff(std::span<const GitCase> cases, std::uint32_t context);

    // diff() at each of `contexts` over one written batch (the files are written once); result k
    // holds the sections at contexts[k]. Errors: as diff().
    [[nodiscard]] Result<std::vector<std::vector<GitSection>>> diffContexts(std::span<const GitCase> cases,
                                                                            std::span<const std::uint32_t> contexts);

    /*
     * Runs the canaries as one -U0 batch and classifies git: Classic when every canary equals its
     * "classic:" list, New when every one equals its "new:" list, otherwise InvalidArgument
     * "unknown git dialect" listing the version (if version() ran) and each canary's hunks.
     * `report` receives one line per canary either way.
     */
    [[nodiscard]] Result<GitDialect> probeDialect(std::span<const DialectGolden> canaries, std::string& report);

    // Writes side a of the cases into a fresh directory, applies `patch` there with
    // `git apply --whitespace=nowarn` (plus --unidiff-zero), and returns every case's file
    // afterwards, in case order. Errors: as diff(), and InvalidArgument when git apply fails.
    [[nodiscard]] Result<std::vector<std::string>> apply(std::span<const GitCase> cases, std::string_view patch,
                                                         bool unidiffZero);

    // Time spent writing batch files and waiting for git, for the ADR's file-writing share.
    [[nodiscard]] std::chrono::nanoseconds writeTime() const noexcept { return m_writeTime; }
    [[nodiscard]] std::chrono::nanoseconds gitTime() const noexcept { return m_gitTime; }

private:
    [[nodiscard]] Result<io::ProcessResult> run(std::vector<std::string> arguments, std::string workingDirectory);
    [[nodiscard]] Result<std::string> freshDirectory(std::string_view prefix);

    io::IProcessLauncher& m_launcher;
    std::string m_git;
    std::string m_scratch;
    std::string m_version;
    std::uint32_t m_runs = 0;
    std::chrono::nanoseconds m_writeTime{0};
    std::chrono::nanoseconds m_gitTime{0};
};

/*
 * Parses the stdout of a diff run over `cases` (exposed for the fault-injection tests). Splits on
 * LF only; returns one section per case. Errors: InvalidArgument per the success criteria.
 */
[[nodiscard]] Result<std::vector<GitSection>> parseGitDiff(std::string_view stdoutBytes,
                                                           std::span<const GitCase> cases);

// git-style output for `cases` given each case's body ("diff --git a/a/cN b/b/cN", "--- ",
// "+++ " and the body), for scripting the fault-injection tests. Empty bodies give no section.
[[nodiscard]] std::string gitStyleOutput(std::span<const std::string> bodies);

} // namespace frost::diff::test
