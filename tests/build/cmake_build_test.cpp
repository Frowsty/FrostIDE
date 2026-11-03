// SPDX-License-Identifier: Apache-2.0
//
// Integration test of the CMake service with the real tools (ADR 0046): the pinned cmake and
// ninja that configured FrostIDE and FrostIDE's own C compiler (clang-cl with the MSVC environment
// captured from vcvarsall.bat on Windows, clang on Linux) configure and build the tiny fixture
// project (fixtures/cmake/tiny) in a scratch copy. One test walks the whole cycle so the tools run
// as few times as possible: configure and the model (targets, the generated source, includes,
// defines), a clean build, a build with a warning and an error mapped to their lines, Clean, the
// reconfigure the real file watcher triggers when CMakeLists.txt changes, and a CMake Error.
// Label "integration" with the tag "cmake". The service's destructor ends every process tree.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/project/cmake_membership.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace frost::build {
namespace {

using namespace std::chrono_literals;

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

std::string native(std::string_view path) {
    auto normalized = io::normalizePath(path);
    EXPECT_TRUE(normalized.has_value()) << path;
    return normalized.value_or(std::string());
}

std::string under(const std::string& root, std::string_view relative) {
    std::string out(relative);
    if constexpr (kWindows) {
        std::ranges::replace(out, '/', '\\');
    }
    return io::joinPath(root, out).value();
}

// Copies the directory tree `from` to `to` (real file system).
void copyTree(const std::string& from, const std::string& to) {
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(to).has_value()) << to;
    auto entries = fs.listDirectory(from);
    ASSERT_TRUE(entries.has_value()) << from;
    for (const io::DirEntry& entry : *entries) {
        const std::string source = io::joinPath(from, entry.name).value();
        const std::string target = io::joinPath(to, entry.name).value();
        if (entry.type == io::FileType::Directory) {
            copyTree(source, target);
        } else {
            auto content = fs.readFile(source);
            ASSERT_TRUE(content.has_value()) << source;
            ASSERT_TRUE(fs.writeFileAtomic(target, *content).has_value()) << target;
        }
    }
}

// Polls `done` until it holds or `timeout` passes.
bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

std::string describeDiagnostics(const CMakeService& service) {
    std::string text;
    for (const BuildDiagnostic& d : service.diagnostics()) {
        text += std::format("\n  {}:{}: {} {}", d.path.empty() ? d.diagnostic.file : d.path, d.diagnostic.line,
                            toString(d.diagnostic.severity), d.diagnostic.message);
    }
    return text;
}

const BuildDiagnostic* findDiagnostic(const std::vector<BuildDiagnostic>& diagnostics, Severity severity,
                                      std::string_view path) {
    const auto found = std::ranges::find_if(diagnostics, [&](const BuildDiagnostic& d) {
        return d.diagnostic.severity == severity && documentKey(d.path) == documentKey(path);
    });
    return found == diagnostics.end() ? nullptr : &*found;
}

/*
 * A real watch of `root` whose batches CMakeService::follow pumps, torn down in file_watcher.hpp's
 * exit order (also when an assertion returns early): stop and join the pump, the watch, the
 * reactor's shutdown, the watcher, the pool. The service must outlive the rig.
 */
class WatchRig {
public:
    WatchRig(CMakeService& service, const std::string& root, const std::string& excluded) {
        auto reactor = io::Reactor::create(io::ReactorOptions{1, "frost-test-cmake"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<sched::ThreadPool>(sched::ThreadPoolOptions{.workerCount = 2});
        auto watcher = io::RealFileWatcher::create(*m_reactor, *m_pool);
        if (!watcher) {
            ADD_FAILURE() << watcher.error().describe();
            return;
        }
        m_watcher = std::move(*watcher);
        io::WatchOptions options;
        options.excluded = {excluded};
        auto watch = sched::syncWait(m_watcher->watch(root, options));
        if (!watch) {
            ADD_FAILURE() << watch.error().describe();
            return;
        }
        m_watch = std::move(*watch);
        m_follow.emplace(m_pool->spawn(sched::Lane::UserVisible, service.follow(*m_watch, m_stop.get_token())));
    }

    ~WatchRig() {
        static_cast<void>(m_stop.request_stop());
        if (m_follow) {
            static_cast<void>(m_follow->join());
        }
        m_watch.reset();
        if (m_reactor) {
            m_reactor->shutdown();
        }
        m_watcher.reset();
        if (m_pool) {
            m_pool->shutdown();
        }
        m_pool.reset();
        m_reactor.reset();
    }
    WatchRig(const WatchRig&) = delete ("owns the reactor");
    WatchRig& operator=(const WatchRig&) = delete ("owns the reactor");

    [[nodiscard]] bool running() const noexcept { return m_follow.has_value(); }

private:
    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<sched::ThreadPool> m_pool;
    std::unique_ptr<io::RealFileWatcher> m_watcher;
    std::unique_ptr<io::IDirectoryWatch> m_watch;
    std::stop_source m_stop;
    std::optional<sched::JoinHandle<Result<void>>> m_follow;
};

TEST(CMakeRealBuild, ConfigureBuildCleanReconfigure) {
    frost::test::ScratchDir scratch("cmake-real");
    const std::string folder = scratch.join("tiny");
    copyTree(native(FROST_BUILD_FIXTURES "/cmake/tiny"), folder);
    auto& fs = io::realFileSystem();
    auto trustStore = project::TrustStore::open(fs, scratch.join("trusted-roots.txt"));
    ASSERT_TRUE(trustStore.has_value()) << trustStore.error().describe();
    ASSERT_TRUE((*trustStore)->trust(folder).has_value());

    CMakeServiceOptions options;
    options.cmakePath = native(FROST_TEST_CMAKE);
    options.ninjaPath = native(FROST_TEST_NINJA);
    options.msvcCacheDirectory = scratch.join("msvc");
    options.reloadDebounce = 300ms;
    options.profileRecords = {project::CMakeProfileRecord{
        .name = "Debug", .cacheVariables = {std::string("CMAKE_C_COMPILER:FILEPATH=") + FROST_TEST_C_COMPILER}}};
    CMakeServiceCallbacks callbacks;
    callbacks.onConfigureOutput = [](BuildOutputKind, std::string_view line) {
        std::cout << "[configure] " << line << '\n';
    };
    callbacks.onOutput = [](BuildOutputKind, std::string_view line) { std::cout << "[build] " << line << '\n'; };
    auto service = CMakeService::create(fs, io::realProcessLauncher(), **trustStore, options, callbacks);

    // Configure (the folder is trusted, so opening it configures) and the model.
    ASSERT_TRUE(service->openFolder(folder).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastConfigure().has_value());
    ASSERT_TRUE(service->lastConfigure()->succeeded())
        << (service->lastConfigure()->error ? service->lastConfigure()->error->describe() : std::string("cmake failed"))
        << describeDiagnostics(*service);
    const auto model = service->model();
    ASSERT_NE(model, nullptr);
    const project::CMakeTarget* core = model->findTarget("core");
    const project::CMakeTarget* app = model->findTarget("app");
    ASSERT_NE(core, nullptr);
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(core->type, project::CMakeTargetType::StaticLibrary);
    EXPECT_EQ(app->type, project::CMakeTargetType::Executable);
    const auto generated =
        std::ranges::find_if(core->sources, [](const project::CMakeSource& s) { return s.generated; });
    ASSERT_NE(generated, core->sources.end());
    EXPECT_EQ(io::fileName(generated->path), "version.c");
    const auto source = std::ranges::find_if(core->sources, [&](const project::CMakeSource& s) {
        return documentKey(s.path) == documentKey(under(folder, "src/core.c"));
    });
    ASSERT_NE(source, core->sources.end());
    EXPECT_FALSE(source->generated);
    ASSERT_TRUE(source->compileGroup.has_value());
    const project::CMakeCompileGroup& group = core->compileGroups[*source->compileGroup];
    EXPECT_EQ(group.language, "C");
    EXPECT_NE(std::ranges::find_if(
                  group.includes,
                  [&](const std::string& p) { return documentKey(p) == documentKey(under(folder, "include")); }),
              group.includes.end());
    EXPECT_NE(std::ranges::find(group.defines, "TINY_CORE=1"), group.defines.end());
    const std::string buildDir = service->activeBuildDirectory();
    EXPECT_EQ(buildDir, under(folder, "cmake-build-debug"));
    EXPECT_TRUE(fs.exists(under(buildDir, "compile_commands.json")).value_or(false));
    const auto inputs = model->projectInputs();
    EXPECT_NE(std::ranges::find_if(inputs,
                                   [&](const std::string& p) {
                                       return documentKey(p) == documentKey(under(folder, "cmake/warnings.cmake"));
                                   }),
              inputs.end());

    // A clean build.
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome().has_value());
    ASSERT_TRUE(service->lastOutcome()->succeeded()) << describeDiagnostics(*service);
    ASSERT_FALSE(app->artifacts.empty());
    EXPECT_TRUE(fs.exists(app->artifacts.front()).value_or(false)) << app->artifacts.front();

    // A warning and an error, at their lines.
    const std::string coreFile = under(folder, "src/core.c");
    ASSERT_TRUE(fs.writeFileAtomic(coreFile, "#include \"core.h\"\n"
                                             "\n"
                                             "int tiny_core(int value) {\n"
                                             "    int unused = 0;\n"
                                             "    return value + TINY_CORE;\n"
                                             "}\n"
                                             "\n"
                                             "int tiny_broken(void) {\n"
                                             "    return tiny_undeclared;\n"
                                             "}\n")
                    .has_value());
    const std::size_t coreIndex = static_cast<std::size_t>(core - model->targets.data());
    ASSERT_TRUE(service->build(BuildAction::Build, coreIndex).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome().has_value());
    EXPECT_FALSE(service->lastOutcome()->succeeded());
    const auto diagnostics = service->diagnostics();
    const BuildDiagnostic* warning = findDiagnostic(diagnostics, Severity::Warning, coreFile);
    const BuildDiagnostic* error = findDiagnostic(diagnostics, Severity::Error, coreFile);
    ASSERT_NE(warning, nullptr) << describeDiagnostics(*service);
    ASSERT_NE(error, nullptr) << describeDiagnostics(*service);
    EXPECT_EQ(warning->diagnostic.line, 4);
    EXPECT_NE(warning->diagnostic.message.find("unused"), std::string::npos);
    EXPECT_EQ(error->diagnostic.line, 9);
    EXPECT_TRUE(error->jumpable);
    EXPECT_NE(error->diagnostic.message.find("tiny_undeclared"), std::string::npos);

    // Clean removes the outputs.
    ASSERT_TRUE(service->build(BuildAction::Clean).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome()->succeeded()) << describeDiagnostics(*service);
    EXPECT_FALSE(fs.exists(app->artifacts.front()).value_or(true));
    ASSERT_TRUE(
        fs.writeFileAtomic(coreFile, "#include \"core.h\"\n\nint tiny_core(int value) { return value + TINY_CORE; }\n")
            .has_value());

    // The real watcher: editing CMakeLists.txt reconfigures by itself.
    WatchRig rig(*service, folder, buildDir);
    ASSERT_TRUE(rig.running());

    const std::string lists = under(folder, "CMakeLists.txt");
    std::string text = fs.readFile(lists).value();
    text += "add_executable(second app/main.c)\ntarget_link_libraries(second PRIVATE core)\n";
    ASSERT_TRUE(fs.writeFileAtomic(lists, text).has_value());
    const bool reconfigured = eventually(
        [&] {
            const auto current = service->model();
            return current != nullptr && current->findTarget("second") != nullptr;
        },
        240s);
    EXPECT_TRUE(reconfigured) << "no reconfigure after the CMakeLists.txt change";
    ASSERT_TRUE(service->waitForIdle(240s));

    // A CMake Error at a known line.
    const std::size_t errorLine = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
    text += "message(FATAL_ERROR \"tiny is broken on purpose\")\n";
    ASSERT_TRUE(fs.writeFileAtomic(lists, text).has_value());
    const bool failed = eventually(
        [&] {
            const auto last = service->lastConfigure();
            return last.has_value() && !last->succeeded() && service->state() == BuildState::Idle;
        },
        240s);
    EXPECT_TRUE(failed) << "no failing reconfigure after the FATAL_ERROR";
    ASSERT_TRUE(service->waitForIdle(240s));
    const auto configureDiagnostics = service->diagnostics();
    const BuildDiagnostic* cmakeError = findDiagnostic(configureDiagnostics, Severity::Error, lists);
    ASSERT_NE(cmakeError, nullptr) << describeDiagnostics(*service);
    EXPECT_EQ(cmakeError->diagnostic.line, static_cast<int>(errorLine));
    EXPECT_NE(cmakeError->diagnostic.message.find("tiny is broken on purpose"), std::string::npos);
    EXPECT_TRUE(cmakeError->jumpable);

    // The rig, then the service (which ends its children), go out of scope in that order.
}

// Owned scratch inputs only: this exercises planning and CMake refresh, not the protected
// transaction or editor admission required before project edits can touch user files.
TEST(CMakeRealBuild, PlansNestedMembershipAndRefreshesTheActualModel) {
    frost::test::ScratchDir scratch("cmake-membership-real");
    auto& fs = io::realFileSystem();
    const auto root = scratch.join("fixture");
    const auto sub = under(root, "sub");
    const auto child = under(sub, "child");
    ASSERT_TRUE(fs.createDirectories(child).has_value());
    const auto rootList = under(root, "CMakeLists.txt");
    const auto subList = under(sub, "CMakeLists.txt");
    const auto childList = under(child, "CMakeLists.txt");
    const auto added = under(sub, "added file.c");
    const auto removed = under(child, "extra file.c");
    const std::string rootBytes =
        "cmake_minimum_required(VERSION 3.20)\nproject(EditFixture C)\nadd_subdirectory(sub)\n";
    const std::string subBytes = "add_library(core STATIC base.c) # target comment\nadd_subdirectory(child)\n";
    const std::string childBytes = "target_sources(core PRIVATE [=[extra file.c]=]) # keep\n"
                                   "target_sources(core PRIVATE other.c)\n";
    ASSERT_TRUE(fs.writeFileAtomic(rootList, rootBytes).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(subList, subBytes).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(childList, childBytes).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(under(sub, "base.c"), "int base(void) { return 0; }\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(added, "int added(void) { return 1; }\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(removed, "int extra(void) { return 2; }\n").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(under(child, "other.c"), "int other(void) { return 3; }\n").has_value());
    auto trust = project::TrustStore::open(fs, scratch.join("trusted-roots.txt"));
    ASSERT_TRUE(trust.has_value());
    ASSERT_TRUE((*trust)->trust(root).has_value());
    CMakeServiceOptions options;
    options.cmakePath = native(FROST_TEST_CMAKE);
    options.ninjaPath = native(FROST_TEST_NINJA);
    options.msvcCacheDirectory = scratch.join("msvc");
    options.profileRecords = {project::CMakeProfileRecord{
        .name = "Debug", .cacheVariables = {std::string("CMAKE_C_COMPILER:FILEPATH=") + FROST_TEST_C_COMPILER}}};
    std::mutex outputMutex;
    std::string capturedOutput;
    CMakeServiceCallbacks callbacks;
    auto capture = [&](BuildOutputKind, std::string_view line) {
        std::lock_guard lock(outputMutex);
        constexpr std::size_t limit = 64 * 1024;
        if (capturedOutput.size() < limit) {
            capturedOutput.append(line.substr(0, limit - capturedOutput.size()));
            if (capturedOutput.size() < limit)
                capturedOutput.push_back('\n');
        }
    };
    callbacks.onOutput = capture;
    callbacks.onConfigureOutput = capture;
    auto service = CMakeService::create(fs, io::realProcessLauncher(), **trust, options, callbacks);
    const auto failureDetails = [&] {
        const auto outcome = service->lastOutcome();
        const auto diagnostics = describeDiagnostics(*service);
        std::lock_guard lock(outputMutex);
        return std::format("\nexit: {} error: {}\n{}{}", outcome ? std::to_string(outcome->result.exitCode) : "missing",
                           outcome && outcome->error ? outcome->error->describe() : "none", diagnostics,
                           capturedOutput);
    };
    ASSERT_TRUE(service->openFolder(root).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastConfigure().has_value() && service->lastConfigure()->succeeded());
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome().has_value() && service->lastOutcome()->succeeded()) << failureDetails();

    const auto snapshots = [&](const project::CMakeProject& model) -> Result<std::vector<project::CMakeInputSnapshot>> {
        std::vector<project::CMakeInputSnapshot> result;
        for (const auto& input : model.inputs) {
            if (input.cmake || std::ranges::any_of(result, [&](const auto& item) {
                    return documentKey(item.path) == documentKey(input.path);
                }))
                continue;
            auto contents = fs.readFile(input.path);
            if (!contents)
                return std::unexpected(std::move(contents.error()));
            result.push_back({.path = input.path, .contents = std::move(*contents)});
        }
        return result;
    };
    const auto findCore = [](const project::CMakeProject& model) {
        const auto found =
            std::ranges::find_if(model.targets, [](const auto& target) { return target.name == "core"; });
        return static_cast<std::size_t>(std::distance(model.targets.begin(), found));
    };
    auto model = service->model();
    ASSERT_NE(model, nullptr);
    auto inputs = snapshots(*model);
    ASSERT_TRUE(inputs.has_value());
    auto add = project::planCMakeMembership(*model, findCore(*model), project::CMakeMembershipOperation::Add, added,
                                            subList, *inputs);
    ASSERT_TRUE(add.has_value()) << add.error().describe();
    EXPECT_EQ(add->original, subBytes);
    EXPECT_EQ(add->replacement, "add_library(core STATIC base.c \"added file.c\") # target comment\n"
                                "add_subdirectory(child)\n");
    ASSERT_TRUE(fs.writeFileAtomic(subList, add->replacement).has_value());
    ASSERT_TRUE(service->configure().has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastConfigure().has_value() && service->lastConfigure()->succeeded());
    model = service->model();
    ASSERT_NE(model, nullptr);
    const auto* core = model->findTarget("core");
    ASSERT_NE(core, nullptr);
    EXPECT_NE(std::ranges::find_if(core->sources,
                                   [&](const auto& item) { return documentKey(item.path) == documentKey(added); }),
              core->sources.end());
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome().has_value() && service->lastOutcome()->succeeded()) << failureDetails();

    inputs = snapshots(*model);
    ASSERT_TRUE(inputs.has_value());
    auto remove = project::planCMakeMembership(*model, findCore(*model), project::CMakeMembershipOperation::Remove,
                                               removed, childList, *inputs);
    ASSERT_TRUE(remove.has_value()) << remove.error().describe();
    EXPECT_EQ(remove->original, childBytes);
    EXPECT_EQ(remove->replacement, "target_sources(core PRIVATE ) # keep\ntarget_sources(core PRIVATE other.c)\n");
    ASSERT_TRUE(fs.writeFileAtomic(childList, remove->replacement).has_value());
    ASSERT_TRUE(service->configure().has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastConfigure().has_value() && service->lastConfigure()->succeeded());
    model = service->model();
    ASSERT_NE(model, nullptr);
    core = model->findTarget("core");
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(std::ranges::find_if(core->sources,
                                   [&](const auto& item) { return documentKey(item.path) == documentKey(removed); }),
              core->sources.end());
    EXPECT_EQ(fs.readFile(rootList).value(), rootBytes);
    EXPECT_EQ(fs.readFile(removed).value(), "int extra(void) { return 2; }\n");
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    ASSERT_TRUE(service->waitForIdle(240s));
    ASSERT_TRUE(service->lastOutcome().has_value() && service->lastOutcome()->succeeded()) << failureDetails();
}

} // namespace
} // namespace frost::build
