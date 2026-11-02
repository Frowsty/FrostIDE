// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests of ClangdService with the real clangd of the pinned LLVM (label "clangd"):
// a CMake-style compile_commands.json, where an undeclared identifier is reported at the right
// line and clears after the fix; the compile commands synthesized for a tiny .vcxproj (include
// directory and defines from its ItemDefinitionGroup), where only the planted error remains; and
// a restart after clangd is killed, after which the open document is diagnosed again. Each test
// starts one clangd (two for the restart) and stops it before returning.

#include "frost/core/log.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::lang::ClangdService;
using frost::lang::ClangdServiceOptions;
using frost::lang::ClangdState;
using frost::lang::DiagnosticSeverity;
using frost::lang::FileDiagnostics;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

// Synthesis options for `configuration` (and system include directories).
frost::lang::SynthesisOptions forConfiguration(std::string configuration, std::vector<std::string> system = {}) {
    frost::lang::SynthesisOptions options;
    options.configuration = std::move(configuration);
    options.systemIncludeDirectories = std::move(system);
    return options;
}

constexpr auto kPatience = 60s; // clangd parses these files in well under a second; this is slack.

std::string fixture(std::string relative) {
    if (kWindows) {
        std::ranges::replace(relative, '/', '\\');
    }
    const std::string base = frost::io::normalizePath(FROST_LANG_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, relative).value()).value();
}

template<class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = kPatience) {
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
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-clangd"});
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

std::string clangdPath() {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

// A service started on `env`; stopped (with the clean shutdown) by stopAndCheck().
std::unique_ptr<ClangdService> startService(Env& env, std::string root,
                                            std::shared_ptr<frost::lang::ICompileCommandsProvider> provider,
                                            frost::lang::RestartPolicy restart = {}) {
    ClangdServiceOptions options;
    options.clangdPath = clangdPath();
    options.workspaceRoot = std::move(root);
    options.launch.jobs = 2;
    options.compileCommands = std::move(provider);
    options.restart = restart;
    auto service = std::make_unique<ClangdService>(frost::io::realProcessLauncher(), env.context(), std::move(options));
    auto started = service->start();
    EXPECT_TRUE(started.has_value()) << started.error().describe();
    return service;
}

void stopAndCheck(Env& env, ClangdService& service) {
    const auto begin = std::chrono::steady_clock::now();
    env.run(service.stop());
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 15s);
    EXPECT_EQ(service.state(), ClangdState::Stopped);
}

bool running(const ClangdService& service) { return service.state() == ClangdState::Running; }

std::vector<frost::lang::LiveDiagnostic> errorsIn(ClangdService& service, const std::string& path) {
    std::vector<frost::lang::LiveDiagnostic> out;
    if (const auto set = service.diagnostics().forFile(path)) {
        for (const auto& d : set->diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) {
                out.push_back(d);
            }
        }
    }
    return out;
}

std::string describe(ClangdService& service) {
    std::string out = std::string("state ") + std::string(frost::lang::toString(service.state()));
    if (auto error = service.lastError()) {
        out += "; last error: " + error->describe();
    }
    for (const std::string& line : service.recentStderr()) {
        out += "\n  " + line;
    }
    return out;
}

// The CMake-style fixture: compile_commands.json under <scratch>/build naming the fixture sources.
void writeCmakeDatabase(const frost::test::ScratchDir& scratch) {
    const std::string source = fixture("cmake_project/src/main.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = scratch.path(),
        .file = source,
        .arguments = {"clang++", "-std=c++20", "-I" + fixture("cmake_project/include"), "-c", source}}};
    ASSERT_TRUE(
        frost::lang::writeCompileCommands(frost::io::realFileSystem(), scratch.join("build"), commands).has_value());
}

frost::text::Document load(const std::string& path) {
    auto text = frost::io::realFileSystem().readFile(path);
    EXPECT_TRUE(text.has_value());
    auto document = frost::text::Document::fromUtf8(text ? *text : std::string());
    EXPECT_TRUE(document.has_value());
    return std::move(*document);
}

TEST(ClangdE2E, CmakeDatabaseReportsAndClearsAnUndeclaredIdentifier) {
    frost::test::ScratchDir scratch("lang-clangd-cmake");
    writeCmakeDatabase(scratch);
    Env env;
    ASSERT_TRUE(env.ok());
    auto provider =
        std::make_shared<frost::lang::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), scratch.path());
    auto service = startService(env, scratch.path(), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    ASSERT_TRUE(service->compileCommands().has_value());
    EXPECT_EQ(service->compileCommands()->directory, scratch.join("build"));
    EXPECT_EQ(service->capabilities()->positionEncoding, frost::lang::PositionEncoding::Utf8);

    const std::string path = fixture("cmake_project/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto errors = errorsIn(*service, path);
    ASSERT_EQ(errors.size(), 1U) << errors.front().message;
    EXPECT_EQ(errors[0].line, 5);
    EXPECT_EQ(errors[0].column, 14);
    EXPECT_EQ(errors[0].code, "undeclared_var_use");
    EXPECT_EQ(errors[0].source, "clang");
    EXPECT_NE(errors[0].message.find("missingName"), std::string::npos);
    ASSERT_TRUE(errors[0].range.has_value());
    EXPECT_EQ(document.text().text(*errors[0].range), "missingName");

    // The fix, sent as an incremental didChange.
    const std::string text = document.text().text();
    const std::size_t at = text.find("missingName");
    auto changes =
        document.replace(frost::text::ByteRange{.begin = frost::ByteOffset{at}, .end = frost::ByteOffset{at + 11}}, "1",
                         frost::text::EditContext{});
    ASSERT_TRUE(changes.has_value());
    ASSERT_TRUE(service->changeDocument(path, *changes, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] {
        const auto set = service->diagnostics().forFile(path);
        return !set || (set->lspVersion == 2 && set->diagnostics.empty());
    })) << describe(*service);
    EXPECT_TRUE(errorsIn(*service, path).empty());
    ASSERT_TRUE(service->saveDocument(path).has_value());
    ASSERT_TRUE(service->closeDocument(path).has_value());
    stopAndCheck(env, *service);
    EXPECT_FALSE(service->serverPid().has_value());
}

TEST(ClangdE2E, SynthesizedVcxprojCommandsCarryIncludesAndDefines) {
    frost::test::ScratchDir scratch("lang-clangd-vcx");
    auto& fs = frost::io::realFileSystem();
    auto model = frost::project::SolutionModel::load(fs, fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    std::function<frost::Result<frost::lang::MsvcEnvironment>()> msvc;
    if constexpr (kWindows) {
        msvc = [&fs] { return frost::lang::findMsvcEnvironment(frost::io::realProcessLauncher(), fs); };
    }
    auto provider = std::make_shared<frost::lang::SolutionCompileCommandsProvider>(
        fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)), scratch.join("ccdb"),
        forConfiguration("Debug|x64"), msvc);
    Env env;
    ASSERT_TRUE(env.ok());
    auto service = startService(env, fixture("msvc_solution"), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    ASSERT_TRUE(service->compileCommands().has_value());
    EXPECT_EQ(service->compileCommands()->source, frost::lang::CompileCommandsSource::Synthesized);

    const std::string path = fixture("msvc_solution/Tiny/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto errors = errorsIn(*service, path);
    std::string all;
    for (const auto& e : errors) {
        all += std::to_string(e.line) + ": " + e.message + "\n";
    }
    // tiny.h was found (no "file not found") and TINY_DEBUG was defined (no #error from tiny.h).
    ASSERT_EQ(errors.size(), 1U) << all;
    EXPECT_EQ(errors[0].line, 5);
    EXPECT_EQ(errors[0].code, "undeclared_var_use");
    EXPECT_NE(errors[0].message.find("notDeclaredAnywhere"), std::string::npos);
    stopAndCheck(env, *service);
}

TEST(ClangdE2E, RestartsAfterAKillAndDiagnosesAgain) {
    frost::test::ScratchDir scratch("lang-clangd-restart");
    writeCmakeDatabase(scratch);
    Env env;
    ASSERT_TRUE(env.ok());
    auto provider =
        std::make_shared<frost::lang::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), scratch.path());
    auto service =
        startService(env, scratch.path(), provider,
                     frost::lang::RestartPolicy{
                         .initialDelay = 100ms, .maxDelay = 1s, .maxConsecutiveCrashes = 3, .stableAfter = 60s});
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    const std::string path = fixture("cmake_project/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto firstPid = service->serverPid();
    ASSERT_TRUE(firstPid.has_value());

    // Clear the store so the republish after the restart is visible.
    service->diagnostics().clear();
    ASSERT_TRUE(service->killServerForTesting().has_value());
    ASSERT_TRUE(waitFor([&] { return service->restarts() == 1 && running(*service); })) << describe(*service);
    EXPECT_NE(service->serverPid(), firstPid);
    ASSERT_TRUE(service->lastError().has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    EXPECT_EQ(errorsIn(*service, path).front().line, 5);
    stopAndCheck(env, *service);
}

} // namespace
