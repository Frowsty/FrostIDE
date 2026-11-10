// SPDX-License-Identifier: Apache-2.0
//
// SLN-5 with this machine's real MSBuild and the pinned clangd (Windows; labels "msbuild" and
// "clangd"), on the synthetic generated solution (tests/project/fixtures/msvc_generated, read
// only): SolutionEvaluation runs GetClCommandLines through the shipped ProjectHost, the
// ClCommandLines provider writes the configuration's database, and the command clangd itself logs
// for Core/src/a.cpp ("ASTWorker building file ... with command") is exactly MSBuild.exe's
// GetClCommandLines for that file (the oracle, tests/project/cl_command_lines_oracle.hpp), turned
// into clang-cl arguments, apart from what clangd adds itself (-resource-dir, --driver-mode, "--").
// In Debug|x64 the NDEBUG block of a.cpp is inactive and the #else block active; Release|x64
// flips both, from clangd's own inactive regions.

#include "frost/build/msbuild_locator.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/cl_command_lines_provider.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/project/cl_command_lines.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "project/cl_command_lines_oracle.hpp"
#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::string generated(std::string relative) {
    std::ranges::replace(relative, '/', '\\');
    const std::string base = frost::io::normalizePath(FROST_PROJECT_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, "msvc_generated\\" + relative).value()).value();
}

template<class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = 120s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The reactor and pool a ClangdService runs on, shut down in the documented order.
class Env {
public:
    Env() {
        frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-gcl-clangd"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~Env() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }
    Env(const Env&) = delete ("owns the reactor");
    Env& operator=(const Env&) = delete ("owns the reactor");

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] frost::io::AsyncContext context() { return frost::io::AsyncContext{*m_reactor, *m_pool}; }
    void run(frost::sched::Task<void> task) { m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join(); }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

// clangd's log lines, thread-safe.
class Log {
public:
    void add(std::string_view line) {
        const std::scoped_lock lock(m_mutex);
        m_lines.emplace_back(line);
    }
    [[nodiscard]] std::vector<std::string> lines() const {
        const std::scoped_lock lock(m_mutex);
        return m_lines;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

/*
 * The argv as clangd's printArgv writes it into the ASTWorker log line (LLVM, Apache-2.0 with LLVM
 * exception; the format only, no code taken): arguments are
 * separated by one space; one with a space, tab, newline, quote or backslash, or a character that
 * is not printable, is written in double quotes with llvm's write_escaped (\\, \", \n, \t, \xHH).
 */
std::vector<std::string> parseClangdArgv(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            ++i;
            continue;
        }
        std::string arg;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    const char next = line[i + 1];
                    if (next == 'x' && i + 3 < line.size()) {
                        arg += static_cast<char>(std::stoi(std::string(line.substr(i + 2, 2)), nullptr, 16));
                        i += 4;
                        continue;
                    }
                    arg += next == 'n' ? '\n' : next == 't' ? '\t' : next;
                    i += 2;
                    continue;
                }
                arg += line[i++];
            }
            ++i; // The closing quote.
        } else {
            while (i < line.size() && line[i] != ' ') {
                arg += line[i++];
            }
        }
        out.push_back(std::move(arg));
    }
    return out;
}

// The command clangd logged for its last build of `file`, or nullopt. The log entry is three
// lines: "ASTWorker building file <file> version <n> with command ", "[<directory>]", the argv.
std::optional<std::vector<std::string>> loggedCommand(const Log& log, std::string_view file) {
    const auto lines = log.lines();
    std::optional<std::vector<std::string>> found;
    for (std::size_t i = 0; i + 2 < lines.size(); ++i) {
        if (lines[i].find("ASTWorker building file") != std::string::npos &&
            lines[i].find(std::string(file)) != std::string::npos && lines[i + 1].starts_with("[")) {
            found = parseClangdArgv(lines[i + 2]);
        }
    }
    return found;
}

bool isClangdAddition(std::string_view arg) {
    return arg.starts_with("-resource-dir") || arg.starts_with("--driver-mode") || arg == "--";
}

std::string joined(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) {
        out += (out.empty() ? "" : " ") + arg;
    }
    return out;
}

TEST(ClCommandLinesClangd, ClangdBuildsAWithMsBuildsGetClCommandLinesAndNdebugFlipsTheInactiveRegion) {
    auto msbuild = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!msbuild) {
        ASSERT_EQ(msbuild.error().code(), frost::ErrorCode::NotFound) << msbuild.error().describe();
        GTEST_SKIP() << "no Visual Studio with MSBuild is installed (vswhere found none)";
    }
    auto& fs = frost::io::realFileSystem();
    const std::string host = frost::io::normalizePath(FROST_PROJECT_HOST).value_or(FROST_PROJECT_HOST);
    ASSERT_TRUE(fs.exists(host).value_or(false)) << host << " was not built (tools/build_project_host.py)";
    auto clangd = frost::lang::findClangd(fs, frost::lang::defaultClangdSearch());
    ASSERT_TRUE(clangd.has_value()) << "the pinned clangd is required: " << clangd.error().describe();
    auto loaded = frost::project::SolutionModel::load(fs, generated("Generated.sln"));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    const auto model = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
    const std::string a = generated("Core/src/a.cpp");
    frost::test::ScratchDir scratch("gcl-clangd");

    for (const char* configuration : {"Debug|x64", "Release|x64"}) {
        SCOPED_TRACE(configuration);
        const bool release = std::string_view(configuration).starts_with("Release");
        frost::project::SolutionEvaluationOptions options{.hostPath = host,
                                                          .msbuildPath = msbuild->msbuildPath,
                                                          .locateMsBuild = {},
                                                          .configuration = configuration,
                                                          .trusted = [] { return true; },
                                                          .environment = {},
                                                          .startTimeout = 120s,
                                                          .requestTimeout = 300s,
                                                          .afterProject = {}};
        options.clCommandLines = true;
        auto evaluation = std::make_shared<frost::project::SolutionEvaluation>(frost::io::realProcessLauncher(), fs,
                                                                               model, std::move(options));
        const std::string directory =
            scratch.join("clcommandlines\\" + frost::lang::configurationDirectoryName(configuration));
        auto provider = std::make_shared<frost::lang::ClCommandLinesCompileCommandsProvider>(fs, evaluation, directory);

        // The oracle: MSBuild.exe's GetClCommandLines for Core in the mapped project configuration.
        const auto index = model->findConfiguration(configuration);
        ASSERT_TRUE(index.has_value());
        frost::project::ProjectId core{0};
        for (std::size_t p = 0; p < model->projects().size(); ++p) {
            if (model->projects()[p].name == "Core") {
                core = frost::project::ProjectId{static_cast<std::uint32_t>(p)};
            }
        }
        const auto& mapping = model->projectConfiguration(core, *index);
        ASSERT_TRUE(mapping.has_value());
        auto oracle = frost::project::test::runClCommandLinesOracle(msbuild->msbuildPath,
                                                                    {.projectPath = model->project(core).path,
                                                                     .configuration = mapping->configuration,
                                                                     .platform = mapping->platform,
                                                                     .solutionPath = model->path(),
                                                                     .itemTypes = {},
                                                                     .metadata = {},
                                                                     .properties = {}});
        ASSERT_TRUE(oracle.has_value());
        std::vector<std::string> expected;
        for (const auto& command : frost::project::clangdCommands(*oracle)) {
            if (frost::io::normalizePath(command.file).value_or("") == a) {
                expected = command.arguments;
            }
        }
        ASSERT_FALSE(expected.empty()) << "the oracle has no command line for " << a;
        EXPECT_EQ(std::ranges::find(expected, "NDEBUG") != expected.end(), release);

        Env env;
        ASSERT_TRUE(env.ok());
        Log log;
        frost::lang::ClangdServiceOptions service;
        service.clangdPath = clangd->path;
        service.workspaceRoot = generated("");
        service.launch.jobs = 2;
        service.launch.logLevel = "info"; // clangd logs the command of each AST build at info.
        service.compileCommands = provider;
        service.onLog = [&log](std::string_view line) { log.add(line); };
        frost::lang::ClangdService server(frost::io::realProcessLauncher(), env.context(), std::move(service));
        ASSERT_TRUE(server.start().has_value());
        ASSERT_TRUE(waitFor([&] {
            return server.state() == frost::lang::ClangdState::Running ||
                   server.state() == frost::lang::ClangdState::Failed;
        }));
        ASSERT_EQ(server.state(), frost::lang::ClangdState::Running) << provider->lastProblem();
        ASSERT_TRUE(server.compileCommands().has_value());
        EXPECT_EQ(server.compileCommands()->source, frost::lang::CompileCommandsSource::ClCommandLines);
        EXPECT_EQ(frost::io::normalizePath(server.compileCommands()->directory).value_or(""),
                  frost::io::normalizePath(directory).value_or("-"));

        auto document = frost::text::Document::fromUtf8(fs.readFile(a).value());
        ASSERT_TRUE(document.has_value());
        const frost::text::Snapshot& snapshot = document->text();
        ASSERT_TRUE(server.openDocument(a, snapshot).has_value());
        ASSERT_TRUE(waitFor([&] { return loggedCommand(log, a).has_value(); })) << "clangd logged no build of " << a;
        std::vector<std::string> used = *loggedCommand(log, a);
        std::vector<std::string> added;
        std::erase_if(used, [&](const std::string& arg) {
            if (isClangdAddition(arg)) {
                added.push_back(arg);
                return true;
            }
            return false;
        });
        // argv[0]: clangd resolves "clang-cl" to a path; the rest must be MSBuild's, token for token.
        ASSERT_FALSE(used.empty());
        EXPECT_TRUE(frost::io::fileName(used.front()).starts_with("clang-cl")) << used.front();
        EXPECT_EQ(std::vector<std::string>(used.begin() + 1, used.end()),
                  std::vector<std::string>(expected.begin() + 1, expected.end()))
            << "clangd: " << joined(used) << "\nMSBuild: " << joined(expected)
            << "\nadded by clangd: " << joined(added);

        // clangd's inactive regions follow NDEBUG.
        ASSERT_TRUE(waitFor([&] {
            const auto regions = server.inactiveRegions(a);
            return regions && !regions->ranges.empty();
        })) << "clangd sent no inactive region for "
            << a;
        const auto regions = *server.inactiveRegions(a);
        const auto inactive = [&](std::string_view name) {
            return std::ranges::any_of(regions.ranges, [&](const frost::text::ByteRange& range) {
                return snapshot.text(range).find(name) != std::string::npos;
            });
        };
        EXPECT_EQ(inactive("coreReleaseOnly"), !release);
        EXPECT_EQ(inactive("coreDebugOnly"), release);
        env.run(server.stop());
        EXPECT_EQ(server.state(), frost::lang::ClangdState::Stopped);
    }
}

} // namespace
