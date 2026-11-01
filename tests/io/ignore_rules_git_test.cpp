// SPDX-License-Identifier: Apache-2.0
//
/*
 * The gitignore matcher and scanTree against git itself (ADR 0020): a scratch repository with a corpus of
 * patterns in .gitignore files at three depths and in .git/info/exclude, and a tree of files that exercise
 * them. Every path's deciding pattern must equal what `git check-ignore --no-index -v -n` reports (source
 * file and line, so negated matches count too), and scanTree's non-ignored files must equal `git ls-files
 * --others --exclude-standard`, both with core.ignorecase false and true. git runs through the real
 * IProcessLauncher with an isolated configuration (GIT_CONFIG_NOSYSTEM, an empty GIT_CONFIG_GLOBAL, HOME
 * and XDG_CONFIG_HOME in the scratch directory). Skipped where git is not installed. The corpus is this
 * suite's own; it is not taken from git's tests.
 */

#include "frost/io/file_system.hpp"
#include "frost/io/ignore_rules.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/tree_scan.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::io::IgnoreOptions;
using frost::io::IgnoreRules;
using frost::io::IgnoreStack;

constexpr char kSep = frost::io::kNativePathStyle == frost::io::PathStyle::Windows ? '\\' : '/';

std::optional<std::string> gitExecutable() {
#ifdef _WIN32
    const std::string candidate = "C:\\Program Files\\Git\\cmd\\git.exe";
#else
    const std::string candidate = "/usr/bin/git";
#endif
    if (frost::io::realFileSystem().exists(candidate).value_or(false)) {
        return candidate;
    }
    return std::nullopt;
}

std::string native(const std::string& root, std::string_view relative) {
    std::string path = root;
    path += kSep;
    for (const char c : relative) {
        path += c == '/' ? kSep : c;
    }
    return path;
}

// The ignore files of the corpus: (path relative to the repository, base directory, content).
struct IgnoreFile {
    std::string path;
    std::string base;
    std::string content;
};

const std::vector<IgnoreFile>& ignoreFiles() {
    static const std::vector<IgnoreFile> files{
        {".git/info/exclude", "", "*.swp\n!sub/x.swp\n# exclude-only comment\nexcluded-dir/\n"},
        {".gitignore", "",
         "# objects\n*.o\n!keep.o\n/rooted.txt\ndocs/*.tmp\n**/logs\ncache/\n!cache/important/\nbuild/**\n"
         "!build/keep/\na/**/z.txt\nfile[0-9].dat\nfile[!0-9].bin\n[[:upper:]]*.up\nsp ace.txt\n\\#hash.txt\n"
         "\\!bang.txt\n*.LOG\nMixed/\nq?.c\ndeep/**/inner/\nx**y\ntrailing.txt   \nesc\\ \\ .txt\r\n"
         "crlf.txt\r\n[broken\n\xC3\xA4*.txt\n!sub/x.swp\n*.tmp/\nm[a-c][x-z]\n"},
        {"sub/.gitignore", "sub", "!*.o\nlocal.txt\n/anchored\nnested/\n*.swp\n!sub/x.swp\n"},
        {"sub/deeper/.gitignore", "sub/deeper", "*.c\n!keep.c\n**/gen\n"},
    };
    return files;
}

// Files of the tree (directories are implied); none differ only in case (Windows).
const std::vector<std::string>& treeFiles() {
    static const std::vector<std::string> files{
        "a.o",
        "keep.o",
        "main.cpp",
        "rooted.txt",
        "sub/rooted.txt",
        "sub/a.o",
        "sub/keep.o",
        "sub/local.txt",
        "sub/anchored",
        "sub/x/anchored",
        "sub/nested/n.txt",
        "sub/x.swp",
        "sub/y.swp",
        "top.swp",
        "sub/deeper/q.c",
        "sub/deeper/keep.c",
        "sub/deeper/m/gen/g.txt",
        "sub/deeper/gen",
        "docs/a.tmp",
        "docs/x/a.tmp",
        "logs/l.txt",
        "a/logs/l.txt",
        "src/logs",
        "cache/c.txt",
        "cache/important/i.txt",
        "build/b.txt",
        "build/keep/k.txt",
        "a/z.txt",
        "a/b/c/z.txt",
        "a/y.txt",
        "file1.dat",
        "fileA.dat",
        "file1.bin",
        "fileA.bin",
        "Upper.up",
        "lower.up",
        "sp ace.txt",
        "#hash.txt",
        "!bang.txt",
        "debug.log",
        "Mixed/m.txt",
        "q1.c",
        "q12.c",
        "deep/inner/f.txt",
        "deep/a/b/inner/f.txt",
        "deep/innerx/f.txt",
        "xay",
        "xy",
        "x/y",
        "trailing.txt",
        "esc  .txt",
        "crlf.txt",
        "[broken",
        "\xC3\xA4"
        "bc.txt",
        "z\xC3\xA4.txt",
        "excluded-dir/e.txt",
        "dir.tmp/t.txt",
        "file.tmp",
        "max",
        "mby",
        "mdz",
    };
    return files;
}

class GitOracle : public ::testing::Test {
protected:
    void SetUp() override {
        auto installed = gitExecutable();
        if (!installed) {
            GTEST_SKIP() << "git is not installed";
        }
        m_git = *installed;
        ASSERT_FALSE(m_scratch.path().empty());
        m_home = native(m_scratch.path(), "home");
        m_repo = native(m_scratch.path(), "repo");
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(m_home).has_value());
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(m_repo).has_value());
        m_config = native(m_home, "gitconfig");
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(m_config, "").has_value());
        const auto init = git({"init", "-q", "."}, {});
        ASSERT_TRUE(init.has_value()) << init.error().describe();
        ASSERT_EQ(init->exitCode, 0) << init->stderrBytes;
        for (const IgnoreFile& file : ignoreFiles()) {
            write(file.path, file.content);
        }
        for (const std::string& file : treeFiles()) {
            write(file, "content");
        }
    }

    void write(std::string_view relative, std::string_view content) {
        const std::string path = native(m_repo, relative);
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(frost::io::parentPath(path)).has_value()) << path;
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(path, content).has_value()) << path;
    }

    frost::Result<frost::io::ProcessResult> git(std::vector<std::string> arguments, std::string_view stdinData,
                                                std::optional<bool> ignoreCase = std::nullopt) {
        frost::io::ProcessSpec spec;
        spec.executable = m_git;
        spec.arguments = {"-C", m_repo};
        if (ignoreCase) {
            spec.arguments.emplace_back("-c");
            spec.arguments.emplace_back(*ignoreCase ? "core.ignorecase=true" : "core.ignorecase=false");
        }
        spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
        spec.environment = {
            {"GIT_CONFIG_NOSYSTEM", "1"},    {"GIT_CONFIG_GLOBAL", m_config}, {"HOME", m_home},
            {"XDG_CONFIG_HOME", m_home},     {"GIT_DIR", std::nullopt},       {"GIT_WORK_TREE", std::nullopt},
            {"GIT_INDEX_FILE", std::nullopt}};
        spec.stdinMode = stdinData.empty() ? frost::io::Stdio::Null : frost::io::Stdio::Pipe;
        return frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, stdinData, 120s);
    }

    // The corpus' rules as one stack (the deeper files only apply below their directories).
    static IgnoreStack stack(bool ignoreCase) {
        IgnoreStack result;
        for (const IgnoreFile& file : ignoreFiles()) {
            result = result.pushed(
                IgnoreRules::parse(file.content, file.base, file.path, IgnoreOptions{.caseInsensitive = ignoreCase}));
        }
        return result;
    }

    // Every file and directory of the tree, relative, with whether it is a directory.
    static std::vector<std::pair<std::string, bool>> allPaths() {
        std::set<std::pair<std::string, bool>> paths;
        for (const std::string& file : treeFiles()) {
            paths.emplace(file, false);
            for (std::size_t slash = file.find('/'); slash != std::string::npos; slash = file.find('/', slash + 1)) {
                paths.emplace(file.substr(0, slash), true);
            }
        }
        for (const IgnoreFile& file : ignoreFiles()) {
            if (!file.path.starts_with(".git/")) {
                paths.emplace(file.path, false);
            }
        }
        return {paths.begin(), paths.end()};
    }

    void compareCheckIgnore(bool ignoreCase) {
        const auto paths = allPaths();
        std::string input;
        for (const auto& [path, isDirectory] : paths) {
            input += path;
            input += '\0';
        }
        const auto result = git({"check-ignore", "--no-index", "-v", "-n", "-z", "--stdin"}, input, ignoreCase);
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        // Exit 0 (some ignored) or 1 (none); anything else is an error.
        ASSERT_LE(result->exitCode, 1) << result->stderrBytes;
        std::vector<std::string> fields;
        std::string_view rest = result->stdoutBytes;
        while (!rest.empty()) {
            const std::size_t nul = rest.find('\0');
            fields.emplace_back(rest.substr(0, nul));
            rest.remove_prefix(nul == std::string_view::npos ? rest.size() : nul + 1);
        }
        ASSERT_EQ(fields.size(), paths.size() * 4) << result->stdoutBytes;
        const IgnoreStack rules = stack(ignoreCase);
        std::size_t matched = 0;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const std::string& source = fields[i * 4];
            const std::string& line = fields[i * 4 + 1];
            const std::string& pattern = fields[i * 4 + 2];
            const std::string& path = fields[i * 4 + 3];
            ASSERT_EQ(path, paths[i].first);
            const auto ours = rules.matchWithParents(path, paths[i].second);
            const std::string ourSource = ours.rules != nullptr ? ours.rules->source() : std::string();
            const std::string ourLine = ours.rule != nullptr ? std::to_string(ours.rule->line) : std::string();
            EXPECT_EQ(ourSource + ":" + ourLine, source + ":" + line)
                << "path '" << path << "' (ignorecase " << ignoreCase << "): git chose '" << pattern << "', we chose '"
                << (ours.rule != nullptr ? ours.rule->text : std::string("<none>")) << "'";
            matched += source.empty() ? 0 : 1;
        }
        EXPECT_GT(matched, paths.size() / 3); // The corpus does exercise the rules.
        std::printf("[oracle] ignorecase %d: %zu paths, %zu decided by a rule\n", ignoreCase ? 1 : 0, paths.size(),
                    matched);
    }

    void compareScan(bool ignoreCase) {
        const auto listed = git({"ls-files", "--others", "--exclude-standard", "-z"}, {}, ignoreCase);
        ASSERT_TRUE(listed.has_value()) << listed.error().describe();
        ASSERT_EQ(listed->exitCode, 0) << listed->stderrBytes;
        std::set<std::string> expected;
        std::string_view rest = listed->stdoutBytes;
        while (!rest.empty()) {
            const std::size_t nul = rest.find('\0');
            expected.emplace(rest.substr(0, nul));
            rest.remove_prefix(nul == std::string_view::npos ? rest.size() : nul + 1);
        }
        frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
        frost::io::ScanOptions options;
        options.ignoreOptions.caseInsensitive = ignoreCase;
        const IgnoreFile& exclude = ignoreFiles().front();
        options.ignore = IgnoreStack().pushed(IgnoreRules::parse(exclude.content, exclude.base, exclude.path,
                                                                 IgnoreOptions{.caseInsensitive = ignoreCase}));
        const auto scanned = pool.spawn(frost::sched::Lane::UserVisible,
                                        frost::io::scanTree(frost::io::realFileSystem(), pool, m_repo, options))
                                 .join();
        pool.shutdown();
        ASSERT_TRUE(scanned.has_value()) << scanned.error().describe();
        std::set<std::string> ours;
        for (const auto& entry : scanned->entries) {
            if (entry.type == frost::io::FileType::Regular) {
                ours.insert(entry.relative);
            }
        }
        EXPECT_EQ(ours, expected) << "ignorecase " << ignoreCase;
        EXPECT_GT(expected.size(), 10U);
        std::printf("[oracle] ignorecase %d: ls-files lists %zu untracked files\n", ignoreCase ? 1 : 0,
                    expected.size());
    }

    frost::test::ScratchDir m_scratch{"gitignore-oracle"};
    std::string m_git;
    std::string m_home;
    std::string m_repo;
    std::string m_config;
};

TEST_F(GitOracle, CheckIgnoreAgreesOnEveryPath) { compareCheckIgnore(false); }

TEST_F(GitOracle, CheckIgnoreAgreesOnEveryPathWithIgnoreCase) { compareCheckIgnore(true); }

TEST_F(GitOracle, ScanTreeListsWhatLsFilesLists) { compareScan(false); }

TEST_F(GitOracle, ScanTreeListsWhatLsFilesListsWithIgnoreCase) { compareScan(true); }

} // namespace
