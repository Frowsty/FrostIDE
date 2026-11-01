// SPDX-License-Identifier: Apache-2.0
//
// The git differential harness shared by diff_git_tests and diff_stress_tests (ADR 0019, D16): the
// git path (FROST_GIT at run time, else the configured FROST_DIFF_GIT), a session with a scratch
// directory, the runner and the probed dialect, and the per-class comparison with the dialect rules.
#pragma once

#include "diff/support/generators.hpp"
#include "diff/support/git_runner.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frost::diff::test {

// The absolute git path: the FROST_GIT environment variable (UTF-8; read with
// GetEnvironmentVariableW on Windows), else the path configured into FROST_DIFF_GIT; empty when
// neither is set.
[[nodiscard]] std::string gitExecutable();

// The failure message when gitExecutable() is empty.
inline constexpr std::string_view kGitNotFound =
    "git not found: configure with -DFROST_GIT_EXECUTABLE=<absolute path> (or set FROST_GIT)";

// A scratch directory, a runner over the real launcher, git's version and its probed dialect.
struct GitSession {
    std::unique_ptr<frost::test::ScratchDir> scratch;
    std::unique_ptr<GitRunner> runner;
    std::string version;
    GitDialect dialect = GitDialect::Classic;
};

// Starts a session; records a test failure and returns nullopt when git is missing, the probe fails
// or the dialect is unknown. Logs the version and the dialect.
[[nodiscard]] std::optional<GitSession> startGitSession(std::string_view tag);

// One generated case with its reproduction seed.
struct SeededCase {
    std::uint64_t seed = 0;
    GitCase pair;
};

// `count` cases of `kind` (Small unless `size` says otherwise), seeded from `master`.
[[nodiscard]] std::vector<SeededCase> generateCases(PairClass kind, std::uint32_t count, std::uint64_t master,
                                                    SizeClass size = SizeClass::Small);

// How a class compared with git.
struct ClassCounts {
    std::uint32_t cases = 0;
    std::uint32_t exact = 0;
    std::uint32_t gitFewer = 0;  // git changed strictly fewer lines (new dialect only).
    std::uint32_t equalCost = 0; // Same number of changed lines, different hunks.
    std::uint32_t gitMore = 0;   // git changed more lines: always a failure.
    std::uint32_t fallbacks = 0; // Our fallback regions over the class.
    std::uint32_t fallbackCases = 0;
    std::vector<bool> matched; // Per case: our -U0 hunks equal git's.
};

/*
 * Diffs every case with frost::diff (LineSplit::Lf) and with git at -U0, and applies the dialect
 * rules (ADR 0019, D6): exact hunks on Classic git; on New git exact, or git's deleted + inserted
 * total below ours (git-fewer: counted and printed with its seed; the caller caps how many there
 * may be, see NewDialectFewer). On either dialect a case with git's total above ours fails, and
 * so does an equal-cost case with different hunks. With `totalsOnly` (dense fallback, where git's
 * Myers variant and ours pick different minimal scripts) only the totals are compared: equal on
 * Classic, git <= ours on New. Records test failures; returns the counts.
 */
ClassCounts compareWithGit(GitSession& session, std::string_view name, const std::vector<SeededCase>& cases,
                           bool totalsOnly = false);

/*
 * The git-fewer cases measured on the New dialect (git 2.56.0.windows.1, ADR 0019 Measurements),
 * per generator class: `count` of `cases`. On New the tests allow at most twice that rate (plus
 * `slack` cases where the case set differs between build configurations), so a change that makes
 * our hunks coarser fails on Windows too, not only against WSL's Classic git.
 */
struct NewDialectFewer {
    std::uint32_t count = 0;
    std::uint32_t cases = 1;

    // The cap for a run of `total` cases: ceil(2 * count * total / cases) + slack.
    [[nodiscard]] constexpr std::uint32_t cap(std::uint32_t total, std::uint32_t slack = 0) const noexcept {
        const std::uint64_t doubled = std::uint64_t{2} * count * total;
        return static_cast<std::uint32_t>((doubled + cases - 1) / cases) + slack;
    }
};

// One generated case from its own seed (the seed compareWithGit prints), as generateCases makes it.
[[nodiscard]] SeededCase caseFromSeed(PairClass kind, std::uint64_t seed, SizeClass size = SizeClass::Small);

// Our renderUnified bodies at each -U<context> equal git's for every case flagged in `matched`
// (one written batch, one git run per context). An empty `matched` flags the cases whose hunks
// equal git's -U0 hunks; it requires contexts[0] == 0.
void compareUnifiedBodies(GitSession& session, std::string_view name, const std::vector<SeededCase>& cases,
                          const std::vector<bool>& matched, std::span<const std::uint32_t> contexts);

// Prints one summary line per class (for the ADR's measurements).
void printCounts(const GitSession& session, std::string_view name, const ClassCounts& counts);

// Prints the session's time writing batch files and waiting for git (the file-writing share).
void printTimes(const GitSession& session, std::string_view test);

} // namespace frost::diff::test
