// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild-evaluated compile-commands provider with this machine's real MSBuild (Windows,
// labels "msbuild" and "clangd"), on the synthetic fixture fixtures/msbuild_props: its include
// directory and define come only from an imported property sheet and an environment variable.
// The tests check the evaluated commands, fresh evaluation after environment and property-sheet changes and that a real
// clangd given those commands reports only the planted error. A machine without Visual Studio skips them. The fixture
// is copied to a scratch directory because one test rewrites the .props.

#include "frost/build/msbuild_locator.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::lang::MsBuildCompileCommandsProvider;

std::string fixture(std::string relative) {
    std::ranges::replace(relative, '/', '\\');
    const std::string base = frost::io::normalizePath(FROST_LANG_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, relative).value()).value();
}

bool copyTree(frost::io::IFileSystem& fs, const std::string& from, const std::string& to) {
    if (!fs.createDirectories(to)) {
        return false;
    }
    auto entries = fs.listDirectory(from);
    if (!entries) {
        return false;
    }
    for (const frost::io::DirEntry& entry : *entries) {
        const std::string source = frost::io::joinPath(from, entry.name).value_or("");
        const std::string target = frost::io::joinPath(to, entry.name).value_or("");
        if (entry.type == frost::io::FileType::Directory) {
            if (!copyTree(fs, source, target)) {
                return false;
            }
            continue;
        }
        auto bytes = fs.readFile(source);
        if (!bytes || !fs.writeFileAtomic(target, *bytes)) {
            return false;
        }
    }
    return true;
}

// This machine's MSBuild.exe, or nullopt (the test skips) when no Visual Studio is installed.
std::optional<std::string> machineMsBuild() {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!found) {
        if (found.error().code() != frost::ErrorCode::NotFound) {
            ADD_FAILURE() << found.error().describe();
        }
        return std::nullopt;
    }
    return found->msbuildPath;
}

// A copy of fixtures/msbuild_props and a provider over it whose MSBuild gets FROST_TEST_SDK_ROOT.
class PropsSolution {
public:
    explicit PropsSolution(std::string_view tag) : m_scratch(tag) {
        EXPECT_TRUE(copyTree(frost::io::realFileSystem(), fixture("msbuild_props"), m_scratch.join("src")));
        auto model = frost::project::SolutionModel::load(frost::io::realFileSystem(), sln());
        EXPECT_TRUE(model.has_value()) << model.error().describe();
        if (model) {
            m_model = std::make_shared<const frost::project::SolutionModel>(std::move(*model));
        }
    }

    [[nodiscard]] std::string sln() const { return m_scratch.join("src\\Props.sln"); }
    [[nodiscard]] std::string mainCpp() const { return m_scratch.join("src\\App\\src\\main.cpp"); }
    [[nodiscard]] std::string props() const { return m_scratch.join("src\\sdk\\Sdk.props"); }
    [[nodiscard]] std::string sdkRoot() const { return m_scratch.join("src\\sdk"); }
    [[nodiscard]] std::string cache() const { return m_scratch.join("cache"); }
    [[nodiscard]] const std::shared_ptr<const frost::project::SolutionModel>& model() const { return m_model; }

    [[nodiscard]] frost::lang::MsBuildProviderOptions options(std::string msbuild) const {
        frost::lang::MsBuildProviderOptions options;
        options.configuration = "Debug|x64";
        options.msbuildPath = std::move(msbuild);
        options.trusted = [] { return true; };
        options.environment = {{"FROST_TEST_SDK_ROOT", sdkRoot()}};
        options.timeout = 120s;
        return options;
    }

private:
    frost::test::ScratchDir m_scratch;
    std::shared_ptr<const frost::project::SolutionModel> m_model;
};

// The arguments of `file`'s command in the database `location` names.
std::optional<std::vector<std::string>> argumentsFor(const frost::lang::CompileCommandsLocation& location,
                                                     const std::string& file) {
    auto text = frost::io::realFileSystem().readFile(location.file);
    EXPECT_TRUE(text.has_value()) << location.file;
    auto json = text ? frost::lang::parseJson(*text) : frost::Result<frost::lang::Json>();
    if (!text || !json) {
        return std::nullopt;
    }
    for (const frost::lang::Json& command : json->items()) {
        if (frost::lang::documentKey(command.stringAt("file")) != frost::lang::documentKey(file)) {
            continue;
        }
        std::vector<std::string> out;
        if (const frost::lang::Json* arguments = command.find("arguments"); arguments != nullptr) {
            for (const frost::lang::Json& argument : arguments->items()) {
                if (const std::string* value = argument.asString(); value != nullptr) {
                    out.push_back(*value);
                }
            }
        }
        return out;
    }
    return std::nullopt;
}

bool has(const std::vector<std::string>& args, std::string_view value) {
    return std::ranges::find(args, value) != args.end();
}

std::string joined(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) {
        out += arg + ' ';
    }
    return out;
}

TEST(MsBuildEvaluationE2E, PropsSheetAndEnvironmentReachFreshCommands) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild on this machine";
    }
    PropsSolution solution("lang-msbuild-eval");
    ASSERT_NE(solution.model(), nullptr);
    MsBuildCompileCommandsProvider provider(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                            solution.model(), solution.cache(), solution.options(*msbuild));
    auto first = provider.locate();
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ((*first)->source, frost::lang::CompileCommandsSource::Evaluated);
    EXPECT_EQ(provider.lastStats().evaluated, 1u);
    EXPECT_TRUE(provider.warnings().empty());
    const auto args = argumentsFor(**first, solution.mainCpp()).value_or(std::vector<std::string>{});
    ASSERT_FALSE(args.empty());
    EXPECT_TRUE(has(args, "/I" + solution.sdkRoot() + "\\include")) << joined(args);
    EXPECT_TRUE(has(args, "/DFROST_SDK_FEATURE=1")) << joined(args);
    EXPECT_TRUE(has(args, "/std:c++20")) << joined(args);
    EXPECT_TRUE(has(args, "/Zc:__cplusplus")) << joined(args);
    // The evaluated IncludePath brings the MSVC and Windows SDK headers as system includes.
    EXPECT_TRUE(std::ranges::any_of(args, [](const std::string& arg) { return arg.starts_with("/imsvc"); }))
        << joined(args);

    // Re-evaluate: MSBuild inputs include more than the previously discovered files.
    auto second = provider.locate();
    ASSERT_TRUE(second.has_value() && second->has_value());
    EXPECT_EQ(provider.lastStats().cached, 0u);
    EXPECT_EQ(provider.lastStats().evaluated, 1u);

    // The sheet changes: evaluated again, with the new define.
    auto& fs = frost::io::realFileSystem();
    std::string sheet = fs.readFile(solution.props()).value();
    const std::string from = "FROST_SDK_FEATURE=1;";
    const std::size_t at = sheet.find(from);
    ASSERT_NE(at, std::string::npos);
    sheet.replace(at, from.size(), "FROST_SDK_FEATURE=1;FROST_SDK_EXTRA=2;");
    ASSERT_TRUE(fs.writeFileAtomic(solution.props(), sheet).has_value());
    auto third = provider.locate();
    ASSERT_TRUE(third.has_value() && third->has_value()) << (third ? "" : third.error().describe());
    EXPECT_EQ(provider.lastStats().evaluated, 1u);
    EXPECT_EQ(provider.lastStats().cached, 0u);
    const auto changed = argumentsFor(**third, solution.mainCpp()).value_or(std::vector<std::string>{});
    EXPECT_TRUE(has(changed, "/DFROST_SDK_EXTRA=2")) << joined(changed);
}

TEST(MsBuildEvaluationE2E, ChangedEnvironmentReevaluatesCommands) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild on this machine";
    PropsSolution solution("lang-msbuild-environment-change");
    ASSERT_NE(solution.model(), nullptr);
    auto options = solution.options(*msbuild);
    MsBuildCompileCommandsProvider first(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                         solution.model(), solution.cache(), options);
    auto initial = first.locate();
    ASSERT_TRUE(initial && initial->has_value());
    const auto initialArgs = argumentsFor(**initial, solution.mainCpp()).value_or(std::vector<std::string>{});
    ASSERT_TRUE(has(initialArgs, "/I" + solution.sdkRoot() + "\\include")) << joined(initialArgs);
    const std::string changedRoot = solution.sdkRoot() + "-changed";
    options.environment = {{"FROST_TEST_SDK_ROOT", changedRoot}};
    MsBuildCompileCommandsProvider second(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                          solution.model(), solution.cache(), options);
    auto updated = second.locate();
    ASSERT_TRUE(updated && updated->has_value());
    EXPECT_NE((*initial)->file, (*updated)->file);
    const auto stillInitial = argumentsFor(**initial, solution.mainCpp()).value_or(std::vector<std::string>{});
    EXPECT_TRUE(has(stillInitial, "/I" + solution.sdkRoot() + "\\include"));
    const auto changed = argumentsFor(**updated, solution.mainCpp()).value_or(std::vector<std::string>{});
    EXPECT_TRUE(has(changed, "/I" + changedRoot + "\\include")) << joined(changed);
    EXPECT_FALSE(has(changed, "/I" + solution.sdkRoot() + "\\include"));
}

TEST(MsBuildEvaluationE2E, ChangedInheritedEnvironmentReevaluatesCommands) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild on this machine";
    PropsSolution solution("lang-msbuild-inherited-env");
    ASSERT_NE(solution.model(), nullptr);
    char* previous = nullptr;
    std::size_t length = 0;
    ASSERT_EQ(_dupenv_s(&previous, &length, "FROST_TEST_SDK_ROOT"), 0);
    const std::optional<std::string> saved = previous ? std::optional<std::string>(previous) : std::nullopt;
    std::free(previous);
    struct Restore {
        std::optional<std::string> value;
        ~Restore() { _putenv_s("FROST_TEST_SDK_ROOT", value ? value->c_str() : ""); }
    } restore{saved};
    ASSERT_EQ(_putenv_s("FROST_TEST_SDK_ROOT", solution.sdkRoot().c_str()), 0);
    auto options = solution.options(*msbuild);
    options.environment.clear();
    MsBuildCompileCommandsProvider provider(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                            solution.model(), solution.cache(), options);
    auto initial = provider.locate();
    ASSERT_TRUE(initial && initial->has_value());
    const auto before = argumentsFor(**initial, solution.mainCpp()).value_or(std::vector<std::string>{});
    ASSERT_TRUE(has(before, "/I" + solution.sdkRoot() + "\\include"));
    const std::string changedRoot = solution.sdkRoot() + "-inherited-change";
    ASSERT_EQ(_putenv_s("FROST_TEST_SDK_ROOT", changedRoot.c_str()), 0);
    auto updated = provider.locate();
    ASSERT_TRUE(updated && updated->has_value());
    const auto args = argumentsFor(**updated, solution.mainCpp()).value_or(std::vector<std::string>{});
    EXPECT_TRUE(has(args, "/I" + changedRoot + "\\include")) << joined(args);
}

TEST(MsBuildEvaluationE2E, NewlyCreatedDirectoryBuildPropsChangesCommands) {
    const auto msbuild = machineMsBuild();
    if (!msbuild)
        GTEST_SKIP() << "no Visual Studio with MSBuild on this machine";
    PropsSolution solution("lang-msbuild-new-import");
    ASSERT_NE(solution.model(), nullptr);
    MsBuildCompileCommandsProvider provider(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                            solution.model(), solution.cache(), solution.options(*msbuild));
    auto initial = provider.locate();
    ASSERT_TRUE(initial && initial->has_value());
    auto path = frost::io::joinPath(frost::io::parentPath(solution.sln()), "Directory.Build.props");
    ASSERT_TRUE(path);
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(
        *path, "<Project><ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>"
               "NEW_DIRECTORY_PROP=1;%(PreprocessorDefinitions)</PreprocessorDefinitions>"
               "</ClCompile></ItemDefinitionGroup></Project>"));
    auto updated = provider.locate();
    ASSERT_TRUE(updated && updated->has_value());
    const auto args = argumentsFor(**updated, solution.mainCpp()).value_or(std::vector<std::string>{});
    EXPECT_TRUE(has(args, "/DNEW_DIRECTORY_PROP=1")) << joined(args);
}

// The reactor and pool a ClangdService runs on, shut down in the documented order.
class Env {
public:
    Env() {
        frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-msbuild-clangd"});
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

template<class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = 60s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

TEST(MsBuildEvaluationE2E, ClangdWithEvaluatedCommandsReportsOnlyThePlantedError) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild on this machine";
    }
    PropsSolution solution("lang-msbuild-clangd");
    ASSERT_NE(solution.model(), nullptr);
    auto& fs = frost::io::realFileSystem();
    auto chain = frost::lang::solutionCompileCommandsChain(frost::io::realProcessLauncher(), fs, solution.model(),
                                                           std::string(frost::io::parentPath(solution.sln())),
                                                           solution.cache(), solution.options(*msbuild));
    auto clangd = frost::lang::findClangd(fs, frost::lang::defaultClangdSearch());
    ASSERT_TRUE(clangd.has_value()) << clangd.error().describe();
    Env env;
    ASSERT_TRUE(env.ok());
    frost::lang::ClangdServiceOptions options;
    options.clangdPath = clangd->path;
    options.workspaceRoot = std::string(frost::io::parentPath(solution.sln()));
    options.launch.jobs = 2;
    options.compileCommands = chain;
    frost::lang::ClangdService service(frost::io::realProcessLauncher(), env.context(), std::move(options));
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitFor([&] {
        return service.state() == frost::lang::ClangdState::Running ||
               service.state() == frost::lang::ClangdState::Failed;
    }));
    ASSERT_EQ(service.state(), frost::lang::ClangdState::Running);
    ASSERT_TRUE(service.compileCommands().has_value());
    EXPECT_EQ(service.compileCommands()->source, frost::lang::CompileCommandsSource::Evaluated);

    const std::string path = solution.mainCpp();
    auto document = frost::text::Document::fromUtf8(fs.readFile(path).value());
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(service.openDocument(path, document->text()).has_value());
    const auto errors = [&] {
        std::vector<frost::lang::LiveDiagnostic> out;
        if (const auto set = service.diagnostics().forFile(path)) {
            for (const auto& d : set->diagnostics) {
                if (d.severity == frost::lang::DiagnosticSeverity::Error) {
                    out.push_back(d);
                }
            }
        }
        return out;
    };
    ASSERT_TRUE(waitFor([&] { return !errors().empty(); }));
    const auto found = errors();
    std::string all;
    for (const auto& e : found) {
        all += std::to_string(e.line) + ": " + e.message + "\n";
    }
    // sdk_header.h was found (no "file not found") and FROST_SDK_FEATURE was defined (no #error).
    ASSERT_EQ(found.size(), 1u) << all;
    EXPECT_EQ(found[0].code, "undeclared_var_use");
    EXPECT_NE(found[0].message.find("plantedUndeclared"), std::string::npos);
    env.run(service.stop());
    EXPECT_EQ(service.state(), frost::lang::ClangdState::Stopped);
}

} // namespace
