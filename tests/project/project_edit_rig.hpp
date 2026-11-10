// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the real-tool tests of SLN-6's edits (project_edit_real_test.cpp and the R2a corpus
// test): a scratch git repository holding a copy of a solution, the git diff oracle (`git diff -U0`
// lines of the definitions), MSBuild builds, and runProjectEdit with the shipped ProjectHost.
// Header-only; Windows test code only.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace frost::project::test {

// A process run to its end; a launch failure is a test failure and exit code -1.
inline io::ProcessResult runTool(const std::string& program, std::vector<std::string> arguments,
                                 const std::string& directory,
                                 std::chrono::seconds timeout = std::chrono::seconds(600)) {
    io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    spec.environment = {
        {"MSBUILDDISABLENODEREUSE", "1"}, {"MSBUILDTERMINALLOGGER", "off"}, {"GIT_TERMINAL_PROMPT", "0"}};
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, timeout);
    EXPECT_TRUE(result.has_value()) << program << ": " << (result ? "" : result.error().describe());
    return result ? std::move(*result) : io::ProcessResult{.exitCode = -1};
}

/*
 * A scratch git repository under the system temp directory (removed with the test): `copy`
 * fills it, commit() records the state the next diff is taken against. Every git call disables
 * line-ending conversion, so diffs are byte-for-byte.
 */
class EditScratch {
public:
    explicit EditScratch(std::string_view tag) : m_dir(tag) {
        EXPECT_TRUE(io::realFileSystem().createDirectories(m_dir.path()).has_value());
        auto git = vcs::findGit(io::realFileSystem(), vcs::defaultGitSearch());
        EXPECT_TRUE(git.has_value()) << (git ? "" : git.error().describe());
        m_git = git ? git->path : std::string();
    }

    // git writes its object files read-only; make the tree writable so ScratchDir can remove it.
    ~EditScratch() {
        std::error_code error;
        const std::filesystem::path root(io::toWin32Path(m_dir.path()).value_or(L""));
        for (auto it = std::filesystem::recursive_directory_iterator(root, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            std::error_code ignored;
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, ignored);
        }
    }
    EditScratch(const EditScratch&) = delete ("one scratch tree per test");
    EditScratch& operator=(const EditScratch&) = delete ("one scratch tree per test");

    [[nodiscard]] const std::string& root() const noexcept { return m_dir.path(); }
    [[nodiscard]] std::string join(std::string_view relative) const {
        std::string native(relative);
        for (char& c : native) {
            c = c == '/' ? '\\' : c;
        }
        return m_dir.join(native);
    }

    io::ProcessResult git(std::vector<std::string> arguments) const {
        std::vector<std::string> all = {
            "-c", "core.autocrlf=false",  "-c", "core.safecrlf=false",          "-c", "safe.directory=*",
            "-c", "user.name=frost-test", "-c", "user.email=frost-test@invalid"};
        all.insert(all.end(), arguments.begin(), arguments.end());
        return runTool(m_git, std::move(all), root(), std::chrono::seconds(300));
    }

    // Copies the directory tree `source` into root() and makes it the first commit.
    void copyAndCommit(const std::string& source) {
        std::error_code error;
        std::filesystem::copy(std::filesystem::path(io::toWin32Path(source).value_or(L"")),
                              std::filesystem::path(io::toWin32Path(root()).value_or(L"")),
                              std::filesystem::copy_options::recursive, error);
        ASSERT_FALSE(error) << error.message();
        ASSERT_EQ(git({"init", "-q"}).exitCode, 0);
        commit();
    }

    void commit() {
        ASSERT_EQ(git({"add", "-A"}).exitCode, 0);
        ASSERT_EQ(git({"commit", "-q", "--allow-empty", "-m", "state"}).exitCode, 0);
    }

    /*
     * The oracle of "git diff shows only the intended elements": the changed lines of `paths`
     * (relative, '/'-separated) since the last commit, each "+<line>" or "-<line>" without its
     * line ending, in git's order. Untracked files are not listed.
     */
    std::vector<std::string> changedLines(std::initializer_list<std::string_view> paths) const {
        std::vector<std::string> arguments = {"diff", "--no-color", "--no-ext-diff", "-U0", "--"};
        for (std::string_view path : paths) {
            arguments.emplace_back(path);
        }
        const auto diff = git(std::move(arguments));
        EXPECT_EQ(diff.exitCode, 0) << diff.stderrBytes;
        std::vector<std::string> out;
        std::istringstream in(diff.stdoutBytes);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if ((line.starts_with('+') && !line.starts_with("+++")) ||
                (line.starts_with('-') && !line.starts_with("---"))) {
                out.push_back(std::move(line));
            }
        }
        return out;
    }

    // `git status --porcelain` of the whole tree: empty when nothing changed since the last commit.
    std::string status() const { return git({"status", "--porcelain", "--untracked-files=all"}).stdoutBytes; }

private:
    frost::test::ScratchDir m_dir;
    std::string m_git;
};

// A fresh GUID for a transaction (the helper's journal record is named by it).
inline std::string newTransactionId() {
    static std::atomic<unsigned> counter{0};
    return std::format("{:08x}-5106-4e5d-9a6b-{:012x}", ++counter,
                       static_cast<unsigned long long>(::GetCurrentProcessId()));
}

// Runs `edit` through the shipped helper, accepting its Ready announcement.
inline Result<void> runEdit(const std::string& host, const std::string& msbuild, ProjectEdit edit, bool accept = true) {
    edit.transactionId = newTransactionId();
    return runProjectEdit(io::realProcessLauncher(), ProjectEditLaunch{.hostPath = host, .msbuildPath = msbuild}, edit,
                          [accept](std::string_view digest) { return accept && digest.size() == 64; });
}

// MSBuild.exe `arguments...` in `directory`; returns the result (exit code and output).
inline io::ProcessResult msbuild(const std::string& msbuildPath, std::vector<std::string> arguments,
                                 const std::string& directory) {
    arguments.insert(arguments.end(), {"-nologo", "-nodeReuse:false", "-v:minimal", "-m:1"});
    return runTool(msbuildPath, std::move(arguments), directory);
}

} // namespace frost::project::test
