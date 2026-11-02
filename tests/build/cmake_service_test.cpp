// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for CMakeService (cmake_service.hpp) over a MemFs folder and the scripted launcher:
// nothing runs before trust (an existing reply is still shown), configure after trust (the query,
// the command line, the reply read, CMake warnings and errors as diagnostics), Build, Clean and
// Rebuild of a target with the compiler diagnostics resolved against the build directory, the
// debounced reconfigure on a CMake file change, and profiles (settings records, persistence).
// The synthetic File API reply comes from tests/project/fixtures/cmake/reply.

#include "frost/build/cmake_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/value.hpp"

#include "build_test_util.hpp"
#include "project/cmake_reply_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::build {
namespace {

using test::kWindows;
using test::MemTree;
using namespace std::chrono_literals;

constexpr auto kWait = 20s;

std::string exe(std::string_view name) { return kWindows ? std::string(name) + ".exe" : std::string(name); }

// What the callbacks saw, under a lock (they run on the service thread).
struct Events {
    mutable Mutex mutex;
    std::vector<BuildState> states FROST_GUARDED_BY(mutex);
    std::vector<std::vector<std::string>> trustRequests FROST_GUARDED_BY(mutex);
    std::vector<std::string> configureLines FROST_GUARDED_BY(mutex);
    std::vector<std::string> buildLines FROST_GUARDED_BY(mutex);
    std::vector<CMakeConfigureOutcome> configures FROST_GUARDED_BY(mutex);
    std::vector<CMakeBuildOutcome> builds FROST_GUARDED_BY(mutex);
    int models FROST_GUARDED_BY(mutex) = 0;
    int profileChanges FROST_GUARDED_BY(mutex) = 0;

    CMakeServiceCallbacks callbacks() {
        CMakeServiceCallbacks out;
        out.onStateChanged = [this](BuildState state) {
            LockGuard lock(mutex);
            states.push_back(state);
        };
        out.onNeedsTrust = [this](const std::vector<std::string>& roots) {
            LockGuard lock(mutex);
            trustRequests.push_back(roots);
        };
        out.onConfigureOutput = [this](BuildOutputKind, std::string_view line) {
            LockGuard lock(mutex);
            configureLines.emplace_back(line);
        };
        out.onOutput = [this](BuildOutputKind, std::string_view line) {
            LockGuard lock(mutex);
            buildLines.emplace_back(line);
        };
        out.onConfigureFinished = [this](const CMakeConfigureOutcome& outcome) {
            LockGuard lock(mutex);
            configures.push_back(outcome);
        };
        out.onFinished = [this](const CMakeBuildOutcome& outcome) {
            LockGuard lock(mutex);
            builds.push_back(outcome);
        };
        out.onModelChanged = [this](const std::shared_ptr<const project::CMakeProject>&) {
            LockGuard lock(mutex);
            ++models;
        };
        out.onProfilesChanged = [this](const std::vector<project::CMakeProfile>&, std::optional<std::size_t>) {
            LockGuard lock(mutex);
            ++profileChanges;
        };
        return out;
    }
};

class CMakeServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tree.write("tiny/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Tiny C)\n");
        tree.write("tiny/src/core.c", "int core(void) { return 0; }\n");
        tree.write("sdk/" + exe("cmake"), "x");
        tree.write("sdk/" + exe("ninja"), "x");
        auto opened = project::TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        trustStore = std::move(*opened);
    }

    void TearDown() override { service.reset(); }

    CMakeServiceOptions options() const {
        CMakeServiceOptions out;
        out.cmakePath = cmake();
        out.ninjaPath = ninja();
        out.pathVariable = "unused";
        out.useVisualStudio = false;
        out.captureMsvcEnvironment = false;
        out.stateFilePath = MemTree::path("data/cmake-profiles.txt");
        out.reloadDebounce = 30ms;
        return out;
    }

    void start(CMakeServiceOptions serviceOptions) {
        service = CMakeService::create(tree.fs, launcher, *trustStore, std::move(serviceOptions), events.callbacks());
    }
    void start() { start(options()); }

    static std::string folder() { return MemTree::path("tiny"); }
    static std::string buildDir(std::string_view relative = {}) {
        return relative.empty() ? MemTree::path("tiny/cmake-build-debug")
                                : MemTree::path(std::string("tiny/cmake-build-debug/") + std::string(relative));
    }
    static std::string cmake() { return MemTree::path("sdk/" + exe("cmake")); }
    static std::string ninja() { return MemTree::path("sdk/" + exe("ninja")); }

    void installReply() {
        frost::test::installCMakeReply(tree.fs, FROST_CMAKE_REPLY_FIXTURE_DIR, folder(), buildDir(),
                                       MemTree::path("other"));
    }

    // The scripts of a first configure: the version probe, then cmake itself.
    void expectFirstConfigure(io::ScriptedProcess configure = {}) {
        launcher.expect({.expectExecutable = cmake(), .stdoutBytes = "cmake version 4.4.5\n"});
        configure.expectExecutable = cmake();
        launcher.expect(std::move(configure));
    }

    void waitIdle() { ASSERT_TRUE(service->waitForIdle(kWait)); }

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    std::unique_ptr<project::TrustStore> trustStore;
    Events events;
    std::unique_ptr<CMakeService> service;
};

TEST_F(CMakeServiceTest, NothingRunsBeforeTrust) {
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    EXPECT_EQ(service->state(), BuildState::Idle); // The automatic configure waits for trust silently.
    ASSERT_EQ(service->profiles().size(), 1U);
    EXPECT_EQ(service->activeProfile()->name, "Debug");
    EXPECT_EQ(service->activeBuildDirectory(), buildDir());
    EXPECT_EQ(service->model(), nullptr);

    ASSERT_TRUE(service->configure().has_value()); // Reload asks for trust instead.
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_EQ(service->untrustedRoots(), std::vector<std::string>{folder()});
    EXPECT_EQ(service->configure().error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    service->cancel();
    waitIdle();
    EXPECT_EQ(service->state(), BuildState::Idle);
    ASSERT_TRUE(service->lastConfigure().has_value());
    EXPECT_TRUE(service->lastConfigure()->result.cancelled);
    {
        LockGuard lock(events.mutex);
        ASSERT_EQ(events.trustRequests.size(), 1U);
        EXPECT_EQ(events.trustRequests[0], std::vector<std::string>{folder()});
    }

    // An existing reply is shown before trust, still without running anything.
    installReply();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->model()->targets.size(), 4U);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_FALSE(tree.fs.exists(buildDir(".cmake/api/v1/query/client-frostide/query.json")).value_or(false));
}

TEST_F(CMakeServiceTest, ConfiguresOnceTrusted) {
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    installReply(); // As if cmake wrote it.
    ASSERT_TRUE(service->configure().has_value());
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    expectFirstConfigure({.stdoutBytes = "-- Configuring done (0.1s)\n",
                          .stderrBytes = "CMake Warning at CMakeLists.txt:2 (message):\n  Be careful.\n\n"});
    ASSERT_TRUE(service->trust(folder()).has_value());
    waitIdle();

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_EQ(launches[0].spec.arguments, std::vector<std::string>{"--version"});
    const io::ProcessSpec& spec = launches[1].spec;
    EXPECT_EQ(spec.workingDirectory, folder());
    const std::vector<std::string> expected = {"-S",
                                               folder(),
                                               "-B",
                                               buildDir(),
                                               "-G",
                                               "Ninja",
                                               "-DCMAKE_BUILD_TYPE=Debug",
                                               "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + ninja(),
                                               "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    EXPECT_EQ(spec.arguments, expected);
    if constexpr (kWindows) {
        EXPECT_NE(
            std::ranges::find(spec.environment, std::pair<std::string, std::optional<std::string>>{"VSLANG", "1033"}),
            spec.environment.end());
    }
    EXPECT_TRUE(tree.fs.exists(buildDir(".cmake/api/v1/query/client-frostide/query.json")).value_or(false));

    auto outcome = service->lastConfigure();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->succeeded());
    EXPECT_TRUE(outcome->modelLoaded);
    EXPECT_EQ(outcome->counts.warnings, 1U);
    ASSERT_NE(service->model(), nullptr);
    EXPECT_NE(service->model()->findTarget("core"), nullptr);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].diagnostic.severity, Severity::Warning);
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/CMakeLists.txt"));
    EXPECT_EQ(diagnostics[0].diagnostic.line, 2);
    EXPECT_TRUE(diagnostics[0].jumpable);
    const auto watched = service->watchedFiles();
    EXPECT_NE(std::ranges::find(watched, MemTree::path("tiny/cmake/warnings.cmake")), watched.end());
    EXPECT_NE(std::ranges::find(watched, MemTree::path("tiny/app/CMakeLists.txt")), watched.end());
    {
        LockGuard lock(events.mutex);
        EXPECT_NE(std::ranges::find(events.configureLines, "-- Configuring done (0.1s)"), events.configureLines.end());
        EXPECT_TRUE(events.buildLines.empty());
        ASSERT_EQ(events.configures.size(), 1U);
    }
}

TEST_F(CMakeServiceTest, ReportsCMakeErrors) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    start();
    expectFirstConfigure({.exitCode = 1,
                          .stderrBytes = "CMake Error at CMakeLists.txt:2 (message):\n  The tiny project is broken.\n\n"
                                         "-- Configuring incomplete, errors occurred!\n"});
    ASSERT_TRUE(service->openFolder(folder()).has_value()); // Trusted: configures at once.
    waitIdle();
    auto outcome = service->lastConfigure();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_EQ(outcome->counts.errors, 1U);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].diagnostic.severity, Severity::Error);
    EXPECT_EQ(diagnostics[0].diagnostic.message, "The tiny project is broken.");
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/CMakeLists.txt"));
    EXPECT_EQ(diagnostics[0].diagnostic.line, 2);
    EXPECT_EQ(service->model(), nullptr);
}

TEST_F(CMakeServiceTest, BuildsCleansAndRebuildsTargets) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    start();
    int saves = 0;
    service->setSaveAllHook([&saves](SaveAllCompletion done) {
        ++saves;
        done({});
    });
    expectFirstConfigure();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    const auto& targets = service->model()->targets;
    const auto core =
        static_cast<std::size_t>(std::ranges::find(targets, "core", &project::CMakeTarget::name) - targets.begin());

    const std::string output = "[1/2] Building C object CMakeFiles/core.dir/src/core.c.o\n"
                               "../src/core.c:1:22: warning: unused variable 'x' [-Wunused-variable]\n"
                               "../src/core.c:1:30: error: use of undeclared identifier 'y'\n"
                               "ninja: build stopped: subcommand failed.\n";
    launcher.expect({.expectExecutable = cmake(), .exitCode = 1, .stdoutBytes = output});
    std::optional<CMakeBuildOutcome> completed;
    ASSERT_TRUE(
        service
            ->build(BuildAction::Build, core, [&completed](const CMakeBuildOutcome& outcome) { completed = outcome; })
            .has_value());
    waitIdle();
    ASSERT_TRUE(completed.has_value());
    EXPECT_FALSE(completed->succeeded());
    ASSERT_NE(completed->model, nullptr);
    EXPECT_NE(completed->model->findTarget("core"), nullptr);
    auto failed = service->lastOutcome();
    ASSERT_TRUE(failed.has_value());
    EXPECT_FALSE(failed->succeeded());
    EXPECT_EQ(failed->request.targetName, "core");
    EXPECT_EQ(failed->counts.errors, 1U);
    EXPECT_EQ(failed->counts.warnings, 1U);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 2U);
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/src/core.c")); // Resolved against the build directory.
    EXPECT_EQ(diagnostics[0].diagnostic.code, "-Wunused-variable");
    EXPECT_TRUE(diagnostics[1].jumpable);
    EXPECT_EQ(diagnostics[1].diagnostic.column, 30);
    EXPECT_EQ(service->diagnosticsForDocument(MemTree::path("tiny/src/core.c")).size(), 2U);

    launcher.expect({.expectExecutable = cmake()});
    ASSERT_TRUE(service->build(BuildAction::Clean).has_value());
    waitIdle();
    EXPECT_TRUE(service->lastOutcome()->succeeded());
    EXPECT_TRUE(service->diagnostics().empty()); // Each job starts clean.

    launcher.expect({.expectExecutable = cmake()});
    launcher.expect({.expectExecutable = cmake()});
    ASSERT_TRUE(service->build(BuildAction::Rebuild, core).has_value());
    waitIdle();
    EXPECT_TRUE(service->lastOutcome()->succeeded());

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 6U);
    EXPECT_EQ(launches[2].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "core"}));
    EXPECT_EQ(launches[2].spec.workingDirectory, buildDir());
    EXPECT_EQ(launches[3].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "clean"}));
    EXPECT_EQ(launches[4].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "clean"}));
    EXPECT_EQ(launches[5].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "core"}));
    EXPECT_EQ(saves, 3);
    EXPECT_EQ(service->build(BuildAction::Build, 99).error().code(), ErrorCode::InvalidArgument);
    LockGuard lock(events.mutex);
    EXPECT_EQ(events.builds.size(), 3U);
    EXPECT_NE(std::ranges::find(events.buildLines, "../src/core.c:1:30: error: use of undeclared identifier 'y'"),
              events.buildLines.end());
}

TEST_F(CMakeServiceTest, ReconfiguresWhenACMakeFileChanges) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    start();
    expectFirstConfigure();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_EQ(launcher.launches().size(), 2U);

    // Unrelated files and the build tree do not reconfigure.
    service->filesChanged(
        {.changes = {io::FileChange{.kind = io::FileChangeKind::Modified, .path = MemTree::path("tiny/src/core.c")},
                     io::FileChange{.kind = io::FileChangeKind::Modified, .path = buildDir("CMakeCache.txt")}},
         .sequence = 1});
    waitIdle();
    EXPECT_EQ(launcher.launches().size(), 2U);
    EXPECT_FALSE(service->modelStale());

    // An included *.cmake file does, once, after the quiet period (the tools are known by now).
    launcher.expect({.expectExecutable = cmake()});
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/cmake/warnings.cmake")}},
                           .sequence = 2});
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/CMakeLists.txt")}},
                           .sequence = 3});
    waitIdle();
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 3U);
    EXPECT_EQ(launches[2].spec.arguments.front(), "-S");
    EXPECT_FALSE(service->modelStale());
    LockGuard lock(events.mutex);
    EXPECT_EQ(events.configures.size(), 2U);
}

TEST_F(CMakeServiceTest, ProfilesFromSettingsArePersisted) {
    settings::RecordList records = {
        settings::Record{settings::RecordField{.name = "name", .value = std::string("Debug")}},
        // Fields sorted by name, as the settings store keeps them.
        settings::Record{settings::RecordField{.name = "buildType", .value = std::string("Release")},
                         settings::RecordField{.name = "cacheVariables", .value = settings::StringList{"A=1"}},
                         settings::RecordField{.name = "jobs", .value = std::int64_t{2}},
                         settings::RecordField{.name = "name", .value = std::string("Release")}},
        settings::Record{settings::RecordField{.name = "buildType", .value = std::string("Nameless")}},
    };
    const auto converted = cmakeProfileRecords(records);
    ASSERT_EQ(converted.size(), 2U);
    EXPECT_EQ(converted[1].buildType, "Release");
    EXPECT_EQ(converted[1].jobs, 2);
    EXPECT_EQ(converted[1].cacheVariables, std::vector<std::string>{"A=1"});

    CMakeServiceOptions withRecords = options();
    withRecords.profileRecords = converted;
    start(withRecords);
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    ASSERT_EQ(service->profiles().size(), 2U);
    EXPECT_EQ(service->activeProfileIndex(), std::optional<std::size_t>(0));
    ASSERT_TRUE(service->setActiveProfile("Release").has_value());
    EXPECT_EQ(service->activeBuildDirectory(), MemTree::path("tiny/cmake-build-release"));
    EXPECT_EQ(service->setActiveProfile("Nope").error().code(), ErrorCode::NotFound);
    waitIdle();
    EXPECT_TRUE(launcher.launches().empty()); // Untrusted: switching runs nothing.

    // A new service (a restart) picks the persisted profile.
    service.reset();
    start(withRecords);
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    EXPECT_EQ(service->activeProfile()->name, "Release");
    ASSERT_TRUE(service->setProfileRecords({}, project::CMakeProfileSource::ApplicationSettings).has_value());
    ASSERT_EQ(service->profiles().size(), 1U); // Back to the default profile.
    EXPECT_EQ(service->activeProfile()->name, "Debug");
    EXPECT_EQ(service->openFolder(MemTree::path("nowhere")).error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(service->closeFolder().has_value());
    EXPECT_EQ(service->activeBuildDirectory(), "");
    EXPECT_EQ(service->configure().error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace frost::build
