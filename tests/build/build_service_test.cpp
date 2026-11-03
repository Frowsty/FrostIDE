// SPDX-License-Identifier: Apache-2.0
//
// Tests for BuildService (frost/build/build_service.hpp) on every platform, over a MemFs solution
// and the scripted launcher: the active configuration (default, choice, persistence), what a
// solution or project build asks MSBuild for, Busy, the trust gate, the Save-All hook (order,
// failure, cancel while saving), diagnostics with absolute paths and per-document queries,
// progress, cancel while running, and the order of the callbacks.

#include "frost/build/build_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::build {
namespace {

using namespace std::chrono_literals;
using project::ProjectId;
using test::MemTree;

constexpr auto kWait = 30s;

#ifdef _WIN32
const std::string kMsBuild = "C:\\VS\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe";
#else
const std::string kMsBuild = "/opt/vs/MSBuild/Current/Bin/amd64/MSBuild.exe";
#endif

std::string vcxproj(std::string_view items) {
    return std::string(R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
)") + std::string(items) +
           "</Project>\n";
}

/*
 * Game.sln under <root>\Game: Release|x64 comes first, so the Debug default is not just "the
 * first". Core sits in the solution folder Engine; App references Core; Tool builds in Debug
 * only; Shared lives outside the solution's directory (a second trust root).
 */
void writeGame(MemTree& tree) {
    constexpr std::string_view kCpp = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";
    const auto project = [kCpp](std::string_view name, std::string_view path, std::string_view guid) {
        return std::format("Project(\"{}\") = \"{}\", \"{}\", \"{}\"\r\nEndProject\r\n", kCpp, name, path, guid);
    };
    const auto both = [](std::string_view guid) {
        return std::format(
            "\t\t{0}.Release|x64.ActiveCfg = Release|x64\r\n\t\t{0}.Release|x64.Build.0 = Release|x64\r\n"
            "\t\t{0}.Debug|x64.ActiveCfg = Debug|x64\r\n\t\t{0}.Debug|x64.Build.0 = Debug|x64\r\n",
            guid);
    };
    constexpr std::string_view kCore = "{00000000-0000-4000-8000-000000000001}";
    constexpr std::string_view kApp = "{00000000-0000-4000-8000-000000000002}";
    constexpr std::string_view kTool = "{00000000-0000-4000-8000-000000000003}";
    constexpr std::string_view kShared = "{00000000-0000-4000-8000-000000000004}";
    constexpr std::string_view kEngine = "{00000000-0000-4000-8000-0000000000F1}";
    std::string sln = "Microsoft Visual Studio Solution File, Format Version 12.00\r\n";
    sln += std::format("Project(\"{{2150E333-8FDC-42A3-9474-1A3956D46DE8}}\") = \"Engine\", \"Engine\", \"{}\"\r\n"
                       "EndProject\r\n",
                       kEngine);
    sln += project("Core", "Engine\\Core\\Core.vcxproj", kCore);
    sln += project("App", "App\\App.vcxproj", kApp);
    sln += project("Tool", "Tool\\Tool.vcxproj", kTool);
    sln += project("Shared", "..\\Shared\\Shared.vcxproj", kShared);
    sln += "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
           "\t\tRelease|x64 = Release|x64\r\n\t\tDebug|x64 = Debug|x64\r\n\tEndGlobalSection\r\n"
           "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n";
    sln += both(kCore) + both(kApp) + both(kShared);
    sln += std::format("\t\t{0}.Release|x64.ActiveCfg = Release|x64\r\n\t\t{0}.Debug|x64.ActiveCfg = Debug|x64\r\n"
                       "\t\t{0}.Debug|x64.Build.0 = Debug|x64\r\n",
                       kTool);
    sln += std::format("\tEndGlobalSection\r\n\tGlobalSection(NestedProjects) = preSolution\r\n\t\t{} = {}\r\n"
                       "\tEndGlobalSection\r\nEndGlobal\r\n",
                       kCore, kEngine);
    tree.write("Game/Game.sln", sln);
    tree.write("Game/Engine/Core/Core.vcxproj",
               vcxproj("  <ItemGroup><ClCompile Include=\"core.cpp\" /></ItemGroup>\n"));
    tree.write("Game/Engine/Core/core.cpp", "int core() { return 1; }\n");
    tree.write("Game/App/App.vcxproj",
               vcxproj("  <ItemGroup><ClCompile Include=\"main.cpp\" /></ItemGroup>\n"
                       "  <ItemGroup><ProjectReference Include=\"..\\Engine\\Core\\Core.vcxproj\" /></ItemGroup>\n"));
    tree.write("Game/App/main.cpp", "int main() { return x; }\n");
    tree.write("Game/Tool/Tool.vcxproj", vcxproj(""));
    tree.write("Shared/Shared.vcxproj", vcxproj(""));
}

// What the scripted starter saw: every MSBuild request, in order.
struct Starts {
    Mutex mutex;
    std::vector<MsBuildRequest> requests FROST_GUARDED_BY(mutex);
};

// Records the service's callbacks as strings, under one mutex.
struct Recorder {
    Mutex mutex;
    std::vector<std::string> events FROST_GUARDED_BY(mutex);
    std::vector<std::string> output FROST_GUARDED_BY(mutex);
    std::vector<std::vector<std::string>> trustRequests FROST_GUARDED_BY(mutex);
    std::vector<BuildOutcome> finished FROST_GUARDED_BY(mutex);
    std::vector<BuildProgress> progress FROST_GUARDED_BY(mutex);
    std::vector<std::string> configurations FROST_GUARDED_BY(mutex);

    void note(std::string event) {
        LockGuard lock(mutex);
        events.push_back(std::move(event));
    }

    BuildServiceCallbacks callbacks() {
        return {
            .onStateChanged = [this](BuildState state) { note(std::string("state ") + std::string(toString(state))); },
            .onNeedsTrust =
                [this](const std::vector<std::string>& roots) {
                    LockGuard lock(mutex);
                    trustRequests.push_back(roots);
                },
            .onOutput =
                [this](BuildOutputKind kind, std::string_view line) {
                    LockGuard lock(mutex);
                    output.push_back(std::string(kind == BuildOutputKind::Status ? "status: " : "") +
                                     std::string(line));
                },
            .onDiagnostic = [this](const BuildDiagnostic& d) { note("diagnostic " + d.diagnostic.code); },
            .onDiagnosticsCleared = [this] { note("cleared"); },
            .onProgress =
                [this](const BuildProgress& value) {
                    LockGuard lock(mutex);
                    progress.push_back(value);
                },
            .onFinished =
                [this](const BuildOutcome& outcome) {
                    note("finished");
                    LockGuard lock(mutex);
                    finished.push_back(outcome);
                },
            .onActiveConfigurationChanged =
                [this](const project::SolutionConfiguration& configuration) {
                    LockGuard lock(mutex);
                    configurations.push_back(configuration.name());
                },
        };
    }
};

// A MemFs with Game.sln, a trust store on it, the scripted launcher and a service whose builds
// run as scripted BuildSessions.
class ServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        writeGame(tree);
        auto opened = project::TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        trustStore = std::move(*opened);
        auto loaded = project::SolutionModel::load(tree.fs, MemTree::path("Game/Game.sln"));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        solution = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    }

    void TearDown() override { service.reset(); }

    [[nodiscard]] BuildServiceOptions options() {
        return {.stateFilePath = MemTree::path("data/active-configurations.txt"),
                .msbuildPath = kMsBuild,
                .vsInstanceId = {},
                .maxCpuCount = 2,
                .startBuild = [this](const MsBuildRequest& request,
                                     BuildCallbacks callbacks) -> Result<std::unique_ptr<BuildSession>> {
                    recorder.note("start");
                    {
                        LockGuard lock(starts.mutex);
                        starts.requests.push_back(request);
                    }
                    // msbuildProcessSpec takes Windows paths only, so the scripted tool gets a plain
                    // spec; the command line is checked separately (commandLineOf), on Windows.
                    io::ProcessSpec spec;
                    spec.executable = request.msbuildPath;
                    spec.arguments = {request.solutionPath};
                    return BuildSession::start(launcher, spec, request.trusted, std::move(callbacks));
                }};
    }

    // Why a build that should have succeeded did not, for the failure message.
    [[nodiscard]] static std::string why(const BuildOutcome& outcome) {
        return outcome.error ? outcome.error->describe() : std::format("exit code {}", outcome.result.exitCode);
    }

    void createService() {
        service = BuildService::create(tree.fs, launcher, *trustStore, options(), recorder.callbacks());
        ASSERT_TRUE(service->openSolution(solution).has_value());
    }

    void trustAll() {
        ASSERT_TRUE(trustStore->trust(MemTree::path("Game")).has_value());
        ASSERT_TRUE(trustStore->trust(MemTree::path("Shared")).has_value());
    }

    [[nodiscard]] ProjectId projectNamed(std::string_view name) const {
        for (std::uint32_t i = 0; i < solution->projects().size(); ++i) {
            if (solution->project(ProjectId{i}).name == name) {
                return ProjectId{i};
            }
        }
        ADD_FAILURE() << "no project " << name;
        return ProjectId{0};
    }

    [[nodiscard]] std::vector<MsBuildRequest> requests() {
        LockGuard lock(starts.mutex);
        return starts.requests;
    }

    [[nodiscard]] std::vector<std::string> events() {
        LockGuard lock(recorder.mutex);
        return recorder.events;
    }

    [[nodiscard]] BuildOutcome finishedOutcome() {
        EXPECT_TRUE(service->waitForIdle(kWait));
        LockGuard lock(recorder.mutex);
        EXPECT_FALSE(recorder.finished.empty());
        return recorder.finished.empty() ? BuildOutcome{} : recorder.finished.back();
    }

    // Waits for the build to end and expects it to have succeeded.
    void expectSucceeded() {
        const BuildOutcome outcome = finishedOutcome();
        EXPECT_TRUE(outcome.succeeded()) << why(outcome);
    }

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    std::unique_ptr<project::TrustStore> trustStore;
    std::shared_ptr<const project::SolutionModel> solution;
    Recorder recorder;
    Starts starts;
    std::unique_ptr<BuildService> service;
};

TEST_F(ServiceTest, ProjectEditExcludesBuildAndPublishesBeforeRelease) {
    createService();
    ASSERT_TRUE(service->setActiveConfiguration("Release|x64"));
    ASSERT_TRUE(service->beginProjectEdit());
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{0}).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->openSolution(solution).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->closeSolution().error().code(), ErrorCode::Busy);
    ASSERT_TRUE(service->finishProjectEdit(solution));
    EXPECT_FALSE(service->projectEditInProgress());
    ASSERT_TRUE(service->activeConfiguration());
    EXPECT_EQ(service->activeConfiguration()->name(), "Release|x64");
    ASSERT_TRUE(service->beginProjectEdit());
    const auto rejected = service->finishProjectEdit(nullptr);
    ASSERT_FALSE(rejected);
    EXPECT_TRUE(service->projectEditInProgress());
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    service->cancelProjectEdit();
    EXPECT_FALSE(service->projectEditInProgress());
}

TEST_F(ServiceTest, ExternalLaunchReservationExcludesCompetingOperationsByGeneration) {
    createService();
    auto first = service->reserveExternalLaunch();
    ASSERT_TRUE(first);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->beginProjectEdit().error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->reserveRun().error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{0}).error().code(), ErrorCode::Busy);
    service->releaseExternalLaunch(*first);
    auto second = service->reserveExternalLaunch();
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    service->releaseExternalLaunch(*first);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    service->releaseExternalLaunch(*second);
    ASSERT_TRUE(service->beginProjectEdit());
    EXPECT_EQ(service->reserveExternalLaunch().error().code(), ErrorCode::Busy);
    service->cancelProjectEdit();
    ASSERT_TRUE(service->reserveRun());
    EXPECT_EQ(service->reserveExternalLaunch().error().code(), ErrorCode::Busy);
    service->releaseRun();
}

TEST_F(ServiceTest, ExternalOwnerClosesOnlyAfterItsExactChildrenRelease) {
    createService();
    auto owner = service->reserveExternalLaunch();
    ASSERT_TRUE(owner);
    auto first = service->reserveExternalChild(*owner);
    auto second = service->reserveExternalChild(*owner);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    service->releaseExternalLaunch(*owner);
    EXPECT_FALSE(service->reserveExternalChild(*owner));
    EXPECT_FALSE(service->reserveRun());
    service->releaseExternalChild(*owner + 1, *first);
    service->releaseExternalChild(*owner, *first);
    service->releaseExternalChild(*owner, *first);
    EXPECT_FALSE(service->reserveExternalLaunch());
    service->releaseExternalChild(*owner, *second);
    auto successor = service->reserveExternalLaunch();
    ASSERT_TRUE(successor);
    service->releaseExternalChild(*owner, *second);
    EXPECT_FALSE(service->reserveRun());
    service->releaseExternalLaunch(*successor);
    ASSERT_TRUE(service->reserveRun());
    service->releaseRun();
}

TEST_F(ServiceTest, DefaultsToTheFirstDebugConfigurationAndPersistsTheChoice) {
    createService();
    EXPECT_EQ(service->configurations().size(), 2U);
    EXPECT_EQ(service->activeConfigurationIndex(), 1U); // Debug|x64, though Release|x64 comes first.
    EXPECT_EQ(service->activeConfiguration()->name(), "Debug|x64");

    ASSERT_TRUE(service->setActiveConfiguration("release|X64").has_value());
    EXPECT_EQ(service->activeConfiguration()->name(), "Release|x64");
    EXPECT_EQ(service->setActiveConfiguration("Profile|x64").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{7}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(tree.fs.readFile(MemTree::path("data/active-configurations.txt")).value(),
              "# FrostIDE active configurations v1\n" + MemTree::path("Game/Game.sln") + "\tRelease|x64\n");
    ASSERT_TRUE(service->waitForIdle(kWait));
    {
        LockGuard lock(recorder.mutex);
        EXPECT_EQ(recorder.configurations, (std::vector<std::string>{"Debug|x64", "Release|x64"}));
    }

    // A new service (the next session) restores it.
    service.reset();
    createService();
    EXPECT_EQ(service->activeConfiguration()->name(), "Release|x64");

    // A persisted name the solution no longer has falls back to the default.
    ASSERT_TRUE(tree.fs
                    .writeFileAtomic(MemTree::path("data/active-configurations.txt"),
                                     "# FrostIDE active configurations v1\n" + MemTree::path("Game/Game.sln") +
                                         "\tGone|x64\nnot an entry\n")
                    .has_value());
    ASSERT_TRUE(service->openSolution(solution).has_value());
    EXPECT_EQ(service->activeConfiguration()->name(), "Debug|x64");

    // A corrupt state file restores nothing; a choice still applies and reports the error.
    ASSERT_TRUE(tree.fs.writeFileAtomic(MemTree::path("data/active-configurations.txt"), "garbage\n").has_value());
    ASSERT_TRUE(service->openSolution(solution).has_value());
    EXPECT_EQ(service->activeConfiguration()->name(), "Debug|x64");
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{0}).error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(service->activeConfiguration()->name(), "Release|x64");

    ASSERT_TRUE(service->closeSolution().has_value());
    EXPECT_TRUE(service->configurations().empty());
    EXPECT_FALSE(service->activeConfiguration().has_value());
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{0}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ServiceTest, SolutionAndProjectBuildsAskMsBuildForTheRightTarget) {
    trustAll();
    createService();
    launcher.expect({.stdoutBytes = "  Core.vcxproj -> out\\Core.lib\n"});
    ASSERT_TRUE(service->build(BuildAction::Rebuild).has_value());
    expectSucceeded();

    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build, projectNamed("Core")).has_value());
    const BuildOutcome core = finishedOutcome();
    EXPECT_TRUE(core.succeeded()) << why(core);
    EXPECT_EQ(core.target.projectName, "Engine\\Core");
    ASSERT_TRUE(core.target.projectConfiguration.has_value());
    EXPECT_EQ(core.target.projectConfiguration->configuration, "Debug");

    ASSERT_TRUE(service->setActiveConfiguration("Release|x64").has_value());
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Clean, projectNamed("App")).has_value());
    expectSucceeded();

    const std::vector<MsBuildRequest> seen = requests();
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[0].solutionPath, MemTree::path("Game/Game.sln"));
    EXPECT_EQ(seen[0].msbuildPath, kMsBuild);
    EXPECT_TRUE(seen[0].projectName.empty());
    EXPECT_EQ(seen[0].action, BuildAction::Rebuild);
    EXPECT_EQ(seen[0].configuration, "Debug");
    EXPECT_EQ(seen[0].platform, "x64");
    EXPECT_EQ(seen[0].maxCpuCount, 2U);
    EXPECT_TRUE(seen[0].trusted);
    EXPECT_EQ(seen[1].projectName, "Engine\\Core");
    EXPECT_EQ(seen[1].action, BuildAction::Build);
    EXPECT_EQ(seen[2].projectName, "App");
    EXPECT_EQ(seen[2].configuration, "Release");
    EXPECT_EQ(seen[2].action, BuildAction::Clean);
    EXPECT_EQ(launcher.launches().size(), 3U);
    if constexpr (test::kWindows) {
        // The solution's per-project target reaches MSBuild's command line (Windows paths only).
        EXPECT_EQ(msbuildProcessSpec(seen[0]).value().arguments[1], "-t:Rebuild");
        EXPECT_EQ(msbuildProcessSpec(seen[1]).value().arguments[1], "-t:Engine\\Core");
        EXPECT_EQ(msbuildProcessSpec(seen[2]).value().arguments[1], "-t:App:Clean");
    }

    // Requests the solution cannot build are refused before anything starts.
    EXPECT_EQ(service->build(BuildAction::Build, projectNamed("Tool")).error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(service->build(BuildAction::Build, ProjectId{99}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(requests().size(), 3U);
    EXPECT_EQ(service->state(), BuildState::Idle);
}

TEST_F(ServiceTest, OneBuildAtATime) {
    trustAll();
    createService();
    launcher.expect({.neverExits = true});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    // Busy from the moment build() accepted the request, in every state of the pipeline.
    const auto busy = service->build(BuildAction::Rebuild);
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->setActiveConfiguration(std::size_t{0}).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->openSolution(solution).error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->closeSolution().error().code(), ErrorCode::Busy);
    EXPECT_TRUE(service->currentBuild().has_value());

    // Running: cancel ends MSBuild and the build reports cancelled.
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (service->state() != BuildState::Running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(service->state(), BuildState::Running);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    service->cancel();
    const BuildOutcome outcome = finishedOutcome();
    EXPECT_TRUE(outcome.result.cancelled);
    EXPECT_FALSE(outcome.succeeded());
    EXPECT_TRUE(launcher.launches().at(0).terminated);
    EXPECT_EQ(service->state(), BuildState::Idle);
    EXPECT_FALSE(service->currentBuild().has_value());
    EXPECT_EQ(requests().size(), 1U); // Nothing was queued.

    // Idle again: the next build is accepted.
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    expectSucceeded();
    const std::vector<std::string> seen = events();
    EXPECT_EQ(std::ranges::count(seen, "state Cancelling"), 1);
}

TEST_F(ServiceTest, AnUntrustedSolutionWaitsForTrustThenBuilds) {
    createService();
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_EQ(service->untrustedRoots(), (std::vector<std::string>{MemTree::path("Game"), MemTree::path("Shared")}));
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);

    ASSERT_TRUE(service->trust(MemTree::path("Game")).has_value());
    EXPECT_EQ(service->state(), BuildState::NeedsTrust); // Shared is still untrusted.
    EXPECT_EQ(service->untrustedRoots(), std::vector<std::string>{MemTree::path("Shared")});
    EXPECT_TRUE(requests().empty());

    ASSERT_TRUE(service->trust(MemTree::path("Shared")).has_value());
    expectSucceeded();
    EXPECT_EQ(requests().size(), 1U);
    EXPECT_TRUE(trustStore->isTrusted(MemTree::path("Shared/Shared.vcxproj")));
    {
        LockGuard lock(recorder.mutex);
        ASSERT_EQ(recorder.trustRequests.size(), 2U);
        EXPECT_EQ(recorder.trustRequests[0].size(), 2U);
        EXPECT_EQ(recorder.trustRequests[1], std::vector<std::string>{MemTree::path("Shared")});
    }
    const std::vector<std::string> seen = events();
    const auto needsTrust = std::ranges::find(seen, "state NeedsTrust");
    const auto start = std::ranges::find(seen, "start");
    ASSERT_NE(needsTrust, seen.end());
    ASSERT_NE(start, seen.end());
    EXPECT_LT(needsTrust, start);

    // Cancel while waiting for trust drops the build.
    ASSERT_TRUE(trustStore->untrust(MemTree::path("Shared")).has_value());
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    service->cancel();
    EXPECT_TRUE(finishedOutcome().result.cancelled);
    EXPECT_EQ(requests().size(), 1U);
}

TEST_F(ServiceTest, SaveAllRunsAndFinishesBeforeTheBuildStarts) {
    trustAll();
    createService();
    // The hook completes later, from another thread, as a UI's asynchronous save would.
    std::thread saver;
    service->setSaveAllHook([this, &saver](SaveAllCompletion done) {
        recorder.note("save " + std::string(toString(service->state())));
        saver = std::thread([done = std::move(done), this] {
            std::this_thread::sleep_for(20ms);
            recorder.note("saved");
            done({});
            done(fail(ErrorCode::IoError, "a second completion is ignored"));
        });
    });
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    expectSucceeded();
    saver.join();
    const std::vector<std::string> seen = events();
    const auto save = std::ranges::find(seen, "save SavingDocuments");
    const auto saved = std::ranges::find(seen, "saved");
    const auto start = std::ranges::find(seen, "start");
    ASSERT_NE(save, seen.end());
    ASSERT_NE(start, seen.end());
    EXPECT_LT(save, saved);
    EXPECT_LT(saved, start);
    EXPECT_NE(std::ranges::find(seen, "state Starting"), seen.end());
}

TEST_F(ServiceTest, AFailedSaveAllAbortsTheBuild) {
    trustAll();
    createService();
    service->setSaveAllHook([](SaveAllCompletion done) { done(fail(ErrorCode::IoError, "disk full")); });
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    const BuildOutcome outcome = finishedOutcome();
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->code(), ErrorCode::IoError);
    EXPECT_FALSE(outcome.succeeded());
    EXPECT_FALSE(outcome.result.cancelled);
    EXPECT_TRUE(requests().empty());
    EXPECT_TRUE(launcher.launches().empty());
    LockGuard lock(recorder.mutex);
    ASSERT_FALSE(recorder.output.empty());
    EXPECT_TRUE(recorder.output.back().starts_with("status: Build not started:")) << recorder.output.back();
}

TEST_F(ServiceTest, CancelWhileSavingEndsTheBuildAndALateCompletionIsIgnored) {
    trustAll();
    createService();
    Mutex held;
    SaveAllCompletion pending;
    service->setSaveAllHook([&](SaveAllCompletion done) {
        LockGuard lock(held);
        pending = std::move(done);
    });
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    for (;;) {
        {
            LockGuard lock(held);
            if (pending || std::chrono::steady_clock::now() > deadline) {
                break;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(service->state(), BuildState::SavingDocuments);
    service->cancel();
    EXPECT_TRUE(finishedOutcome().result.cancelled);
    {
        LockGuard lock(held);
        ASSERT_TRUE(pending);
        pending({});
    }
    EXPECT_TRUE(service->waitForIdle(kWait));
    EXPECT_TRUE(requests().empty());
    EXPECT_EQ(service->state(), BuildState::Idle);
}

TEST_F(ServiceTest, DiagnosticsAreResolvedIndexedAndClearedByTheNextBuild) {
    trustAll();
    createService();
    const std::string app = MemTree::path("Game/App/App.vcxproj");
    const std::string main = MemTree::path("Game/App/main.cpp");
    const std::string output = "  Core.vcxproj -> " + MemTree::path("Game/x64/Debug/Core.lib") + "\n" + main +
                               "(1,1): warning C4996: 'getenv': This function may be unsafe. [" + app + "]\n" +
                               "main.cpp(1,21): error C2065: 'x': undeclared identifier [" + app + "]\n" +
                               "LINK : fatal error LNK1104: cannot open file 'Core.lib' [" + app + "]\n";
    launcher.expect({.exitCode = 1, .stdoutBytes = output});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    const BuildOutcome outcome = finishedOutcome();
    EXPECT_FALSE(outcome.succeeded());
    EXPECT_EQ(outcome.counts, (DiagnosticCounts{.errors = 2, .warnings = 1, .messages = 0}));

    const std::vector<BuildDiagnostic> all = service->diagnostics();
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].path, main);
    EXPECT_TRUE(all[0].jumpable);
    EXPECT_EQ(all[1].path, main); // Relative to App's directory.
    EXPECT_TRUE(all[1].jumpable);
    EXPECT_TRUE(all[2].path.empty());
    EXPECT_FALSE(all[2].jumpable);
    EXPECT_EQ(service->diagnosticCounts(), outcome.counts);
    EXPECT_EQ(service->diagnosticsForDocument(main).size(), 2U);
    EXPECT_TRUE(service->diagnosticsForDocument(MemTree::path("Game/Engine/Core/core.cpp")).empty());
    const std::vector<DiagnosticFileGroup> groups = service->diagnosticsByFile();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[0].file, main);
    EXPECT_EQ(groups[1].file, "LINK");

    // Progress: Core reported its output, of the four projects Debug builds (Tool included).
    {
        LockGuard lock(recorder.mutex);
        ASSERT_GE(recorder.progress.size(), 2U);
        EXPECT_EQ(recorder.progress.front(), (BuildProgress{.finishedProjects = 0, .expectedProjects = 4}));
        EXPECT_EQ(recorder.progress.back().finishedProjects, 1U);
        EXPECT_EQ(recorder.progress.back().lastProject, "Core");
        EXPECT_EQ(recorder.progress.back().lastOutput, MemTree::path("Game/x64/Debug/Core.lib"));
        ASSERT_GE(recorder.output.size(), 2U);
        EXPECT_TRUE(
            recorder.output.front().starts_with("status: Build started: solution Game, configuration Debug|x64"))
            << recorder.output.front();
        EXPECT_TRUE(recorder.output.back().starts_with("status: Build failed: 2 errors, 1 warning"))
            << recorder.output.back();
    }
    const std::vector<std::string> seen = events();
    EXPECT_EQ(std::ranges::count(seen, "diagnostic C2065"), 1);
    EXPECT_LT(std::ranges::find(seen, "diagnostic C4996"), std::ranges::find(seen, "finished"));

    // The next build starts with an empty list; building App expects App and Core.
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build, projectNamed("App")).has_value());
    expectSucceeded();
    EXPECT_TRUE(service->diagnostics().empty());
    EXPECT_TRUE(service->diagnosticsForDocument(main).empty());
    EXPECT_EQ(service->progress().expectedProjects, 2U);
    EXPECT_EQ(std::ranges::count(events(), "cleared"), 3); // openSolution and both builds.
}

TEST_F(ServiceTest, StateChangesArriveInPipelineOrderOnTheServiceThread) {
    trustAll();
    std::thread::id callbackThread{};
    bool sameThread = true;
    BuildServiceCallbacks callbacks = recorder.callbacks();
    auto onState = callbacks.onStateChanged;
    callbacks.onStateChanged = [&, onState](BuildState state) {
        const std::thread::id self = std::this_thread::get_id();
        if (callbackThread == std::thread::id{}) {
            callbackThread = self;
        }
        sameThread = sameThread && self == callbackThread;
        onState(state);
    };
    service = BuildService::create(tree.fs, launcher, *trustStore, options(), std::move(callbacks));
    ASSERT_TRUE(service->openSolution(solution).has_value());
    launcher.expect({});
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    expectSucceeded();
    std::vector<std::string> states;
    for (const std::string& event : events()) {
        if (event.starts_with("state ")) {
            states.push_back(event);
        }
    }
    EXPECT_EQ(states,
              (std::vector<std::string>{"state SavingDocuments", "state Starting", "state Running", "state Idle"}));
    EXPECT_TRUE(sameThread);
    EXPECT_NE(callbackThread, std::this_thread::get_id());
}

TEST(BuildServiceNames, SolutionProjectNamesIncludeTheFolders) {
    MemTree tree;
    writeGame(tree);
    auto loaded = project::SolutionModel::load(tree.fs, MemTree::path("Game/Game.sln"));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < loaded->projects().size(); ++i) {
        names.push_back(solutionProjectName(*loaded, ProjectId{i}));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Engine\\Core", "App", "Tool", "Shared"}));
    EXPECT_EQ(toString(BuildState::NeedsTrust), "NeedsTrust");
    auto statePath = BuildService::defaultStatePath();
    if (statePath) {
        EXPECT_TRUE(statePath->ends_with("active-configurations.txt")) << *statePath;
    }
}

} // namespace
} // namespace frost::build
