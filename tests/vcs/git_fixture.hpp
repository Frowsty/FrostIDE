// SPDX-License-Identifier: Apache-2.0
//
// TempRepo: a throwaway git repository in a scratch directory for the vcs tests that run real git
// (label "git"), with git isolated from the machine's configuration, plus helpers to write files,
// run git and make commits. Header-only; test code only.
/*
 * Isolation. Every command (the helpers' and frost::vcs's own) runs with GIT_CONFIG_NOSYSTEM=1 and
 * GIT_CONFIG_GLOBAL pointing at a file in the scratch directory that sets the user, main as the
 * initial branch, core.autocrlf=false and no signing, so the machine's settings (Git for Windows'
 * system autocrlf and credential manager, a user's hooksPath) never change what the tests see.
 * git is found at run time from FROST_GIT, else from the path CMake found (FROST_VCS_GIT).
 * Every process goes through GitRunner, which ends its whole tree before run() returns.
 */
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/vcs/git_repository.hpp"
#include "frost/vcs/git_runner.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::vcs::test {

// The git the tests run (header comment); "" when none was found.
inline std::string testGitPath() {
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableW(L"FROST_GIT", nullptr, 0);
    if (needed > 0) {
        std::wstring value(needed, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"FROST_GIT", value.data(), needed);
        value.resize(length < needed ? length : 0);
        if (auto utf8 = io::fromWide(value); utf8 && !utf8->empty()) {
            return std::move(*utf8);
        }
    }
#else
    if (const char* value = std::getenv("FROST_GIT"); value != nullptr && *value != '\0') {
        return value;
    }
#endif
    return FROST_VCS_GIT;
}

class TempRepo {
public:
    // A scratch directory with an isolated git configuration; init() makes it a repository.
    explicit TempRepo(std::string_view tag, bool trusted = true) : m_scratch(tag) {
        m_fs = &io::realFileSystem();
        m_root = m_scratch.path();
        m_work = join(m_root, "work");
        const std::string config = join(m_root, "gitconfig");
        EXPECT_TRUE(m_fs->createDirectories(m_work).has_value());
        const std::string configText = "[user]\n\tname = Frost Test\n\temail = test@example.invalid\n"
                                       "[init]\n\tdefaultBranch = main\n"
                                       "[core]\n\tautocrlf = false\n"
                                       "[commit]\n\tgpgsign = false\n"
                                       "[protocol \"file\"]\n\tallow = always\n";
        EXPECT_TRUE(m_fs->writeFileAtomic(config, configText).has_value());
        const std::string git = testGitPath();
        EXPECT_FALSE(git.empty()) << "git was not found: configure with -DFROST_GIT_EXECUTABLE=<path> or set FROST_GIT";
        GitRunnerOptions options;
        options.trusted = trusted;
        options.environment = {{"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", config}};
        m_runner = std::make_unique<GitRunner>(io::realProcessLauncher(), git, std::move(options));
    }

    // git writes its object files read-only, which Windows refuses to delete: make the tree
    // writable before the ScratchDir removes it.
    ~TempRepo() { frost::test::makeTreeWritable(m_root); }
    TempRepo(const TempRepo&) = delete ("owns a scratch directory");
    TempRepo& operator=(const TempRepo&) = delete ("owns a scratch directory");

    [[nodiscard]] static std::string join(std::string_view base, std::string_view relative) {
        auto joined = io::joinPath(base, relative);
        EXPECT_TRUE(joined.has_value());
        return joined ? std::move(*joined) : std::string();
    }

    [[nodiscard]] const std::string& root() const noexcept { return m_root; }
    [[nodiscard]] const std::string& work() const noexcept { return m_work; }
    [[nodiscard]] GitRunner& runner() const noexcept { return *m_runner; }
    [[nodiscard]] io::IFileSystem& fs() const noexcept { return *m_fs; }
    // The absolute path of `relative` ('/' separated) in the work tree.
    [[nodiscard]] std::string path(std::string_view relative) const { return absoluteGitPath(m_work, relative); }

    // Runs git in `directory` (the work tree by default); expects exit code `expectedExit`.
    GitOutput git(std::vector<std::string> arguments, int expectedExit = 0, std::string directory = {}) const {
        GitInvocation invocation;
        invocation.arguments = std::move(arguments);
        invocation.workingDirectory = directory.empty() ? m_work : std::move(directory);
        auto output = m_runner->run(invocation);
        EXPECT_TRUE(output.has_value()) << (output ? "" : output.error().describe());
        if (!output) {
            return {};
        }
        EXPECT_EQ(output->exitCode, expectedExit)
            << "git " << invocation.arguments.front() << ": " << output->stdoutBytes << output->stderrBytes;
        return *output;
    }

    // `git init` and the repository's GitRepository.
    void init() {
        git({"init", "-q"});
        auto discovered = discoverRepository(*m_runner, m_work);
        ASSERT_TRUE(discovered.has_value()) << discovered.error().describe();
        ASSERT_TRUE(discovered->has_value());
        m_info = std::move(**discovered);
        m_repository = std::make_unique<GitRepository>(*m_runner, *m_fs, m_info);
    }

    // Uses the existing repository at `work` (a clone) instead of init().
    void open(std::string work) {
        m_work = std::move(work);
        auto discovered = discoverRepository(*m_runner, m_work);
        ASSERT_TRUE(discovered.has_value()) << discovered.error().describe();
        ASSERT_TRUE(discovered->has_value());
        m_info = std::move(**discovered);
        m_repository = std::make_unique<GitRepository>(*m_runner, *m_fs, m_info);
    }

    [[nodiscard]] const RepositoryInfo& info() const noexcept { return m_info; }
    [[nodiscard]] const GitRepository& repository() const noexcept { return *m_repository; }

    void write(std::string_view relative, std::string_view content) const {
        const std::string file = path(relative);
        EXPECT_TRUE(m_fs->createDirectories(std::string(io::parentPath(file))).has_value());
        EXPECT_TRUE(m_fs->writeFileAtomic(file, content).has_value()) << file;
    }

    [[nodiscard]] std::string read(std::string_view relative) const {
        auto bytes = m_fs->readFile(path(relative));
        return bytes ? std::move(*bytes) : std::string("<missing>");
    }

    [[nodiscard]] bool exists(std::string_view relative) const {
        auto found = m_fs->exists(path(relative));
        return found && *found;
    }

    // Stages everything and commits it; returns the new HEAD.
    std::string commitAll(std::string_view message) const {
        git({"add", "-A"});
        git({"commit", "-q", "-m", std::string(message)});
        return std::string(trimGitText(git({"rev-parse", "HEAD"}).stdoutBytes));
    }

    [[nodiscard]] GitStatus status() const {
        auto current = m_repository->status();
        EXPECT_TRUE(current.has_value()) << (current ? "" : current.error().describe());
        return current ? std::move(*current) : GitStatus{};
    }

    // Installs `.git/hooks/<name>` with `body` after a #!/bin/sh line (executable on POSIX).
    void hook(std::string_view name, std::string_view body) const {
        const std::string hooks = join(m_info.gitDir, "hooks");
        EXPECT_TRUE(m_fs->createDirectories(hooks).has_value());
        const std::string file = join(hooks, name);
        EXPECT_TRUE(m_fs->writeFileAtomic(file, std::format("#!/bin/sh\n{}\n", body)).has_value());
#ifndef _WIN32
        std::error_code error;
        std::filesystem::permissions(file, std::filesystem::perms::owner_all, std::filesystem::perm_options::add,
                                     error);
        EXPECT_FALSE(error) << error.message();
#endif
    }

private:
    frost::test::ScratchDir m_scratch;
    io::IFileSystem* m_fs = nullptr;
    std::string m_root;
    std::string m_work;
    std::unique_ptr<GitRunner> m_runner;
    RepositoryInfo m_info;
    std::unique_ptr<GitRepository> m_repository;
};

} // namespace frost::vcs::test
