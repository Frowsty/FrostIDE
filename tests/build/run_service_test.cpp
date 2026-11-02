// SPDX-License-Identifier: Apache-2.0
//
// Tests for RunService (frost/build/run_service.hpp) on every platform, over a MemFs solution, a
// fake builder and the scripted launcher: the startup project (default, choice, persistence), the
// console setting, build before run and no run after a failed or cancelled build, Busy and
// rerun, Stop, input, the console modes' process specs (the external console's cmd wrapper is
// only asserted, never started), the trust check of a run without a build, the property cache
// and the adapter over a real BuildService with a scripted MSBuild.

#include "frost/build/run_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
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
using test::peImage;

constexpr auto kWait = 30s;

#ifdef _WIN32
const std::string kMsBuild = "C:\\VS\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe";
const std::string kCmd = "C:\\Windows\\System32\\cmd.exe";
#else
const std::string kMsBuild = "/opt/vs/MSBuild/Current/Bin/amd64/MSBuild.exe";
const std::string kCmd = "/bin/cmd.exe"; // Never used: the external console needs Windows.
#endif

// Polls `done` every few milliseconds for up to kWait.
bool waitUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

std::string vcxproj(std::string_view type) {
    return std::format(R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Configuration"><ConfigurationType>{}</ConfigurationType></PropertyGroup>
</Project>
)",
                       type);
}

// Run.sln: Core (a static library, first, so the default is not just "the first"), App and Tool
// (programs), in Debug|x64 and Release|x64.
void writeRun(MemTree& tree) {
    constexpr std::string_view kCpp = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";
    const std::vector<std::pair<std::string, std::string>> projects = {
        {"Core", "{00000000-0000-4000-8000-000000000001}"},
        {"App", "{00000000-0000-4000-8000-000000000002}"},
        {"Tool", "{00000000-0000-4000-8000-000000000003}"}};
    std::string sln = "Microsoft Visual Studio Solution File, Format Version 12.00\r\n";
    for (const auto& [name, guid] : projects) {
        sln += std::format("Project(\"{}\") = \"{}\", \"{}\\{}.vcxproj\", \"{}\"\r\nEndProject\r\n", kCpp, name, name,
                           name, guid);
    }
    sln += "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
           "\t\tDebug|x64 = Debug|x64\r\n\t\tRelease|x64 = Release|x64\r\n\tEndGlobalSection\r\n"
           "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n";
    for (const auto& [name, guid] : projects) {
        sln +=
            std::format("\t\t{0}.Debug|x64.ActiveCfg = Debug|x64\r\n\t\t{0}.Debug|x64.Build.0 = Debug|x64\r\n"
                        "\t\t{0}.Release|x64.ActiveCfg = Release|x64\r\n\t\t{0}.Release|x64.Build.0 = Release|x64\r\n",
                        guid);
    }
    sln += "\tEndGlobalSection\r\nEndGlobal\r\n";
    tree.write("Run/Run.sln", sln);
    tree.write("Run/Core/Core.vcxproj", vcxproj("StaticLibrary"));
    tree.write("Run/App/App.vcxproj", vcxproj("Application"));
    tree.write("Run/Tool/Tool.vcxproj", vcxproj("Application"));
}

// The build side: records the builds asked for and finishes them when the test says so.
class FakeBuilder final : public IRunBuilder {
public:
    std::optional<std::size_t> activeConfigurationIndex() const override { return 0; }

    bool building() const override {
        LockGuard lock(m_mutex);
        return m_building;
    }

    Result<void> buildForRun(ProjectId project, BuildCompletion done) override {
        LockGuard lock(m_mutex);
        m_builds.push_back(project);
        if (m_refusal) {
            return std::unexpected(*m_refusal);
        }
        m_pending.push_back(std::move(done));
        return {};
    }

    void cancelBuild() override {
        LockGuard lock(m_mutex);
        ++m_cancels;
    }

    void setBuilding(bool building) {
        LockGuard lock(m_mutex);
        m_building = building;
    }

    void refuseWith(std::optional<Error> error) {
        LockGuard lock(m_mutex);
        m_refusal = std::move(error);
    }

    // Finishes the oldest build that is waiting (after waiting for one to be asked for).
    void finish(bool succeeded, bool cancelled = false) {
        ASSERT_TRUE(waitUntil([this] { return pending() > 0; })) << "no build was asked for";
        BuildCompletion done;
        {
            LockGuard lock(m_mutex);
            done = std::move(m_pending.front());
            m_pending.erase(m_pending.begin());
        }
        BuildOutcome outcome;
        outcome.result.success = succeeded;
        outcome.result.cancelled = cancelled;
        outcome.result.exitCode = succeeded ? 0 : 1;
        outcome.counts.errors = succeeded ? 0 : 2;
        done(outcome);
    }

    [[nodiscard]] std::size_t pending() const {
        LockGuard lock(m_mutex);
        return m_pending.size();
    }

    [[nodiscard]] std::vector<ProjectId> builds() const {
        LockGuard lock(m_mutex);
        return m_builds;
    }

    [[nodiscard]] int cancels() const {
        LockGuard lock(m_mutex);
        return m_cancels;
    }

private:
    mutable Mutex m_mutex;
    bool m_building FROST_GUARDED_BY(m_mutex) = false;
    std::optional<Error> m_refusal FROST_GUARDED_BY(m_mutex);
    std::vector<BuildCompletion> m_pending FROST_GUARDED_BY(m_mutex);
    std::vector<ProjectId> m_builds FROST_GUARDED_BY(m_mutex);
    int m_cancels FROST_GUARDED_BY(m_mutex) = 0;
};

// Records the service's callbacks, under one mutex.
struct Recorder {
    mutable Mutex mutex;
    std::vector<std::string> events FROST_GUARDED_BY(mutex);
    std::string stdoutText FROST_GUARDED_BY(mutex);
    std::string stderrText FROST_GUARDED_BY(mutex);
    std::string systemText FROST_GUARDED_BY(mutex);
    std::vector<RunOutcome> finished FROST_GUARDED_BY(mutex);
    std::vector<std::optional<ProjectId>> startups FROST_GUARDED_BY(mutex);
    std::vector<bool> externals FROST_GUARDED_BY(mutex);

    RunServiceCallbacks callbacks() {
        return {
            .onRunStarted =
                [this](const RunInfo& run) {
                    LockGuard lock(mutex);
                    events.push_back(std::format("started {}", run.projectName));
                },
            .onRunStateChanged =
                [this](const RunInfo& run) {
                    LockGuard lock(mutex);
                    events.push_back(std::string(toString(run.state)));
                },
            .onOutput =
                [this](RunId, RunOutputKind kind, std::string_view text) {
                    LockGuard lock(mutex);
                    (kind == RunOutputKind::Stdout   ? stdoutText
                     : kind == RunOutputKind::Stderr ? stderrText
                                                     : systemText) += text;
                },
            .onRunFinished =
                [this](const RunOutcome& outcome) {
                    LockGuard lock(mutex);
                    events.push_back("finished");
                    finished.push_back(outcome);
                },
            .onStartupProjectChanged =
                [this](std::optional<ProjectId> project) {
                    LockGuard lock(mutex);
                    startups.push_back(project);
                },
            .onExternalConsoleChanged =
                [this](bool external) {
                    LockGuard lock(mutex);
                    externals.push_back(external);
                },
        };
    }

    [[nodiscard]] std::vector<std::string> eventList() const {
        LockGuard lock(mutex);
        return events;
    }

    [[nodiscard]] std::string systemLines() const {
        LockGuard lock(mutex);
        return systemText;
    }
};

class RunServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        writeRun(tree);
        auto opened = project::TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        trustStore = std::move(*opened);
        ASSERT_TRUE(trustStore->trust(MemTree::path("Run")).has_value());
        auto loaded = project::SolutionModel::load(tree.fs, MemTree::path("Run/Run.sln"));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        solution = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    }

    void TearDown() override { service.reset(); }

    // What the fake property source answers: Visual Studio's layout under Run\x64\<configuration>.
    [[nodiscard]] static RunProperties propertiesFor(const RunPropertyRequest& request) {
        std::string name(io::fileName(request.projectPath));
        name.erase(name.rfind('.'));
        RunProperties properties;
        properties.targetDir = MemTree::path(std::format("Run/x64/{}/", request.configuration));
        properties.targetPath = properties.targetDir + name + ".exe";
        properties.projectDir = std::string(io::parentPath(request.projectPath)) + (test::kWindows ? "\\" : "/");
        properties.solutionDir = MemTree::path("Run/");
        properties.configurationType = "Application";
        properties.commandArguments = "--flag \"two words\"";
        properties.environment = "FROST_RUN_TEST=1";
        return properties;
    }

    [[nodiscard]] RunServiceOptions options() {
        return {.stateFilePath = MemTree::path("data/run-settings.txt"),
                .msbuildPath = kMsBuild,
                .vsInstanceId = {},
                .cmdPath = kCmd,
                .properties = [this](const RunPropertyRequest& request) -> Result<RunProperties> {
                    LockGuard lock(requestsMutex);
                    requests.push_back(request);
                    return propertiesFor(request);
                },
                .environment = [](std::string_view) -> std::optional<std::string> { return std::nullopt; },
                .msbuildRunSupported = true,
                .externalConsoleByDefault = false,
                .session = {}};
    }

    void createService(RunServiceOptions serviceOptions) {
        service = RunService::create(tree.fs, launcher, *trustStore, builder, std::move(serviceOptions),
                                     recorder.callbacks());
        ASSERT_TRUE(service->openSolution(solution).has_value());
    }

    void createService() { createService(options()); }

    [[nodiscard]] ProjectId projectNamed(std::string_view name) const {
        for (std::uint32_t i = 0; i < solution->projects().size(); ++i) {
            if (solution->project(ProjectId{i}).name == name) {
                return ProjectId{i};
            }
        }
        ADD_FAILURE() << "no project " << name;
        return ProjectId{0};
    }

    // Writes the program `name` builds to in Debug, as a console (3) or GUI (2) image.
    void writeProgram(std::string_view name, std::uint16_t subsystem = 3) {
        tree.write(std::format("Run/x64/Debug/{}.exe", name), peImage(subsystem));
    }

    [[nodiscard]] static std::string programPath(std::string_view name) {
        return MemTree::path(std::format("Run/x64/Debug/{}.exe", name));
    }

    [[nodiscard]] std::size_t propertyRequests() {
        LockGuard lock(requestsMutex);
        return requests.size();
    }

    [[nodiscard]] RunOutcome lastOutcome() {
        EXPECT_TRUE(service->waitForIdle(kWait));
        auto outcome = service->lastOutcome();
        EXPECT_TRUE(outcome.has_value());
        return outcome.value_or(RunOutcome{});
    }

    // Waits until run `id` is Running.
    void waitForRunning(RunId id) {
        ASSERT_TRUE(waitUntil([&] {
            const auto info = service->runInfo(id);
            return info && info->state == RunState::Running;
        }));
    }

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    std::unique_ptr<project::TrustStore> trustStore;
    std::shared_ptr<const project::SolutionModel> solution;
    FakeBuilder builder;
    Recorder recorder;
    Mutex requestsMutex;
    std::vector<RunPropertyRequest> requests FROST_GUARDED_BY(requestsMutex);
    std::unique_ptr<RunService> service;
};

TEST_F(RunServiceTest, TheStartupProjectDefaultsToTheFirstApplicationAndIsPersisted) {
    createService();
    const ProjectId app = projectNamed("App");
    const ProjectId tool = projectNamed("Tool");
    EXPECT_EQ(service->startupProject(), app) << "Core comes first but is a library";
    EXPECT_EQ(service->startupCandidates().size(), 3U);
    EXPECT_FALSE(service->useExternalConsole());

    ASSERT_TRUE(service->setStartupProject(tool).has_value());
    EXPECT_EQ(service->startupProject(), tool);
    const std::string sln = MemTree::path("Run/Run.sln");
    const std::string toolPath = MemTree::path("Run/Tool/Tool.vcxproj");
    EXPECT_EQ(tree.fs.readFile(MemTree::path("data/run-settings.txt")).value(),
              "# FrostIDE run settings v1\n" + sln + "\t" + toolPath + "\trun-console\n");
    ASSERT_TRUE(service->setUseExternalConsole(true).has_value());
    EXPECT_EQ(tree.fs.readFile(MemTree::path("data/run-settings.txt")).value(),
              "# FrostIDE run settings v1\n" + sln + "\t" + toolPath + "\texternal\n");
    EXPECT_EQ(service->setStartupProject(ProjectId{9}).error().code(), ErrorCode::InvalidArgument);
    ASSERT_TRUE(service->waitForIdle(kWait));
    {
        LockGuard lock(recorder.mutex);
        EXPECT_EQ(recorder.startups, (std::vector<std::optional<ProjectId>>{app, tool}));
        EXPECT_EQ(recorder.externals, (std::vector<bool>{false, true}));
    }

    // The next session restores both.
    service.reset();
    createService();
    EXPECT_EQ(service->startupProject(), tool);
    EXPECT_TRUE(service->useExternalConsole());

    // A persisted project the solution no longer has falls back to the default; junk lines are skipped.
    ASSERT_TRUE(tree.fs
                    .writeFileAtomic(MemTree::path("data/run-settings.txt"),
                                     "# FrostIDE run settings v1\nnot an entry\n" + sln + "\t" +
                                         MemTree::path("Run/Gone/Gone.vcxproj") + "\trun-console\n")
                    .has_value());
    ASSERT_TRUE(service->openSolution(solution).has_value());
    EXPECT_EQ(service->startupProject(), app);
    EXPECT_FALSE(service->useExternalConsole());

    ASSERT_TRUE(service->closeSolution().has_value());
    EXPECT_FALSE(service->startupProject().has_value());
    EXPECT_EQ(service->run().error().code(), ErrorCode::InvalidArgument);
}

TEST_F(RunServiceTest, BuildsFirstAndDoesNotRunAfterAFailedBuild) {
    createService();
    writeProgram("App");
    launcher.expect(
        {.expectExecutable = programPath("App"), .exitCode = 3, .stdoutBytes = "hello\n", .stderrBytes = "oops\n"});

    auto first = service->run();
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    ASSERT_TRUE(waitUntil([&] { return builder.pending() == 1; }));
    EXPECT_EQ(builder.builds(), std::vector<ProjectId>{projectNamed("App")});
    EXPECT_TRUE(launcher.launches().empty()) << "nothing starts before the build is done";
    builder.finish(false);
    RunOutcome failed = lastOutcome();
    EXPECT_TRUE(failed.buildFailed);
    EXPECT_FALSE(failed.ran());
    ASSERT_TRUE(failed.error.has_value());
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_NE(recorder.systemLines().find("Build failed; App was not started."), std::string::npos)
        << recorder.systemLines();
    EXPECT_EQ(propertyRequests(), 0U) << "a failed build does not even ask what to run";

    auto second = service->run();
    ASSERT_TRUE(second.has_value()) << second.error().describe();
    builder.finish(true);
    const RunOutcome ran = lastOutcome();
    ASSERT_TRUE(ran.ran()) << (ran.error ? ran.error->describe() : "");
    EXPECT_EQ(ran.exitCode, 3);
    EXPECT_FALSE(ran.stopped);
    EXPECT_EQ(ran.run.id, *second);
    EXPECT_EQ(ran.run.mode, ConsoleMode::RunConsole);
    {
        LockGuard lock(recorder.mutex);
        EXPECT_EQ(recorder.stdoutText, "hello\n");
        EXPECT_EQ(recorder.stderrText, "oops\n");
        EXPECT_NE(recorder.systemText.find(programPath("App")), std::string::npos);
    }
    EXPECT_EQ(recorder.eventList(),
              (std::vector<std::string>{"started App", "finished", "started App", "Resolving", "Running", "finished"}));

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    const io::ProcessSpec& spec = launches[0].spec;
    EXPECT_EQ(spec.executable, programPath("App"));
    EXPECT_EQ(spec.arguments, (std::vector<std::string>{"--flag", "two words"}));
    EXPECT_EQ(spec.workingDirectory, MemTree::path("Run/App")) << "the default working directory is ProjectDir";
    EXPECT_TRUE(spec.killOnClose);
    EXPECT_EQ(spec.environment.back().first, "FROST_RUN_TEST");
    EXPECT_EQ(spec.environment.back().second, std::optional<std::string>("1"));

    LockGuard lock(requestsMutex);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].projectPath, MemTree::path("Run/App/App.vcxproj"));
    EXPECT_EQ(requests[0].solutionPath, MemTree::path("Run/Run.sln"));
    EXPECT_EQ(requests[0].configuration, "Debug");
    EXPECT_EQ(requests[0].platform, "x64");
    EXPECT_EQ(requests[0].msbuildPath, kMsBuild);
}

TEST_F(RunServiceTest, ASecondRunIsBusyAndRerunStopsAndStartsAgain) {
    createService();
    writeProgram("App");
    launcher.expect({.expectExecutable = programPath("App"), .neverExits = true});
    launcher.expect({.expectExecutable = programPath("App"), .stdoutBytes = "second\n"});

    auto first = service->run();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(service->run().error().code(), ErrorCode::Busy) << "already building";
    builder.finish(true);
    waitForRunning(*first);
    EXPECT_EQ(service->run().error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->activeRun(projectNamed("App")), *first);
    builder.setBuilding(true);
    EXPECT_EQ(service->run(projectNamed("Tool")).error().code(), ErrorCode::Busy) << "a build is in progress";
    builder.setBuilding(false);

    auto again = service->rerun();
    ASSERT_TRUE(again.has_value()) << again.error().describe();
    EXPECT_EQ(*again, *first) << "rerun returns the run it stops";
    builder.finish(true); // The rerun's build.
    const RunOutcome second = lastOutcome();
    EXPECT_EQ(second.exitCode, 0);
    EXPECT_NE(second.run.id, *first);

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_TRUE(launches[0].terminated);
    LockGuard lock(recorder.mutex);
    ASSERT_EQ(recorder.finished.size(), 2U);
    EXPECT_TRUE(recorder.finished[0].stopped);
    EXPECT_EQ(recorder.finished[0].exitCode, io::kTerminatedExitCode);
    EXPECT_EQ(recorder.stdoutText, "second\n");
}

TEST_F(RunServiceTest, StopDuringTheBuildCancelsItAndRunsNothing) {
    createService();
    writeProgram("App");
    auto run = service->run();
    ASSERT_TRUE(run.has_value());
    ASSERT_TRUE(waitUntil([&] { return builder.pending() == 1; }));
    service->stop(*run);
    EXPECT_EQ(builder.cancels(), 1);
    builder.finish(false, true);
    const RunOutcome outcome = lastOutcome();
    EXPECT_TRUE(outcome.stopped);
    EXPECT_TRUE(outcome.buildFailed);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST_F(RunServiceTest, TheRunConsoleWritesInputToTheProgram) {
    createService();
    writeProgram("App");
    launcher.expect({.expectExecutable = programPath("App"), .neverExits = true});
    auto run = service->run();
    ASSERT_TRUE(run.has_value());
    builder.finish(true);
    waitForRunning(*run);
    EXPECT_TRUE(service->runInfo(*run)->acceptsInput);
    ASSERT_TRUE(service->sendInput(*run, "abc\n").has_value());
    ASSERT_TRUE(waitUntil([&] { return launcher.launches().at(0).stdinBytes == "abc\n"; }));
    service->closeInput(*run);
    ASSERT_TRUE(waitUntil([&] { return launcher.launches().at(0).stdinClosed; }));
    EXPECT_FALSE(service->runInfo(*run)->acceptsInput);
    service->stop(*run);
    const RunOutcome outcome = lastOutcome();
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(service->sendInput(*run, "late\n").error().code(), ErrorCode::NotFound);
}

TEST_F(RunServiceTest, ConsoleProgramsGoToTheExternalConsoleAndGuiProgramsGetNone) {
    createService();
    ASSERT_TRUE(service->setUseExternalConsole(true).has_value());
    writeProgram("App");
    // The cmd wrapper is only asserted: the scripted launcher starts nothing, so no window opens.
    launcher.expect({.expectExecutable = test::kWindows ? kCmd : programPath("App")});
    ASSERT_TRUE(service->run().has_value());
    builder.finish(true);
    RunOutcome outcome = lastOutcome();
    ASSERT_TRUE(outcome.ran()) << (outcome.error ? outcome.error->describe() : "");
    auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    if constexpr (test::kWindows) {
        const io::ProcessSpec& spec = launches[0].spec;
        EXPECT_EQ(outcome.run.mode, ConsoleMode::ExternalConsole);
        EXPECT_EQ(spec.executable, kCmd);
        EXPECT_TRUE(spec.arguments.empty());
        ASSERT_TRUE(spec.rawArguments.has_value());
        EXPECT_TRUE(spec.rawArguments->starts_with("/d /s /c \"\"" + programPath("App") + "\" --flag \"two words\" & "))
            << *spec.rawArguments;
        EXPECT_NE(spec.rawArguments->find("& pause & call exit %^FROST_RUN_EXIT%\""), std::string::npos);
        EXPECT_TRUE(spec.newConsole);
        EXPECT_EQ(spec.stdinMode, io::Stdio::Inherit);
        EXPECT_EQ(spec.stdoutMode, io::Stdio::Inherit);
        EXPECT_EQ(spec.stderrMode, io::Stdio::Inherit);
        EXPECT_EQ(spec.workingDirectory, MemTree::path("Run/App"));
    } else {
        EXPECT_EQ(outcome.run.mode, ConsoleMode::RunConsole) << "no external console off Windows";
    }

    // A GUI program gets no console, whatever the setting.
    writeProgram("App", 2);
    launcher.expect({.expectExecutable = programPath("App")});
    ASSERT_TRUE(service->run().has_value());
    builder.finish(true);
    outcome = lastOutcome();
    EXPECT_EQ(outcome.run.mode, ConsoleMode::NoConsole);
    launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_EQ(launches[1].spec.stdinMode, io::Stdio::Null);
    EXPECT_FALSE(launches[1].spec.newConsole);
}

TEST_F(RunServiceTest, AProjectTheConfigurationDoesNotBuildRunsItsLastBuildOnlyInATrustedFolder) {
    builder.refuseWith(Error(ErrorCode::Unsupported, "App is not built in Debug|x64"));
    auto untrusted = project::TrustStore::open(tree.fs, MemTree::path("data/other-roots.txt"));
    ASSERT_TRUE(untrusted.has_value());
    trustStore = std::move(*untrusted);
    createService();
    writeProgram("App");
    ASSERT_TRUE(service->run().has_value());
    RunOutcome outcome = lastOutcome();
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(propertyRequests(), 0U) << "the property query runs project code too";

    ASSERT_TRUE(trustStore->trust(MemTree::path("Run")).has_value());
    launcher.expect({.expectExecutable = programPath("App")});
    ASSERT_TRUE(service->run().has_value());
    outcome = lastOutcome();
    EXPECT_TRUE(outcome.ran()) << (outcome.error ? outcome.error->describe() : "");
    EXPECT_NE(recorder.systemLines().find("App is not built in Debug|x64; running its last build."), std::string::npos)
        << recorder.systemLines();
}

TEST_F(RunServiceTest, AMissingProgramIsReportedNotStarted) {
    createService();
    ASSERT_TRUE(service->run().has_value());
    builder.finish(true);
    const RunOutcome outcome = lastOutcome();
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->code(), ErrorCode::NotFound);
    EXPECT_FALSE(outcome.buildFailed);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_NE(recorder.systemLines().find("App was not started"), std::string::npos) << recorder.systemLines();
}

TEST_F(RunServiceTest, PropertiesAreCachedUntilTheProjectOrUserFileChanges) {
    createService();
    writeProgram("App");
    for (int i = 0; i < 2; ++i) {
        launcher.expect({.expectExecutable = programPath("App")});
        ASSERT_TRUE(service->run().has_value());
        builder.finish(true);
        EXPECT_TRUE(lastOutcome().ran());
    }
    EXPECT_EQ(propertyRequests(), 1U);

    tree.clock.advance(5s);
    tree.write("Run/App/App.vcxproj.user", "<Project />\n");
    launcher.expect({.expectExecutable = programPath("App")});
    ASSERT_TRUE(service->run().has_value());
    builder.finish(true);
    EXPECT_TRUE(lastOutcome().ran());
    EXPECT_EQ(propertyRequests(), 2U);

    service->invalidateProperties();
    launcher.expect({.expectExecutable = programPath("App")});
    ASSERT_TRUE(service->run().has_value());
    builder.finish(true);
    EXPECT_TRUE(lastOutcome().ran());
    EXPECT_EQ(propertyRequests(), 3U);
}

TEST_F(RunServiceTest, MsBuildSolutionsAreUnsupportedWhereTheyCannotRun) {
    RunServiceOptions unsupported = options();
    unsupported.msbuildRunSupported = false;
    createService(std::move(unsupported));
    EXPECT_EQ(service->run().error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(builder.builds().empty());
}

TEST_F(RunServiceTest, TheBuildServiceAdapterBuildsBeforeTheRun) {
    // A real BuildService whose MSBuild is scripted: the launches show the order.
    BuildServiceOptions buildOptions{
        .stateFilePath = MemTree::path("data/active-configurations.txt"),
        .msbuildPath = kMsBuild,
        .vsInstanceId = {},
        .maxCpuCount = 2,
        .startBuild = [this](const MsBuildRequest& request,
                             BuildCallbacks callbacks) -> Result<std::unique_ptr<BuildSession>> {
            io::ProcessSpec spec;
            spec.executable = request.msbuildPath;
            spec.arguments = {request.solutionPath};
            return BuildSession::start(launcher, spec, request.trusted, std::move(callbacks));
        }};
    auto buildService = BuildService::create(tree.fs, launcher, *trustStore, std::move(buildOptions));
    ASSERT_TRUE(buildService->openSolution(solution).has_value());
    const auto adapter = makeRunBuilder(*buildService);
    service = RunService::create(tree.fs, launcher, *trustStore, *adapter, options(), recorder.callbacks());
    ASSERT_TRUE(service->openSolution(solution).has_value());
    writeProgram("App");

    launcher.expect({.expectExecutable = kMsBuild, .exitCode = 1});
    ASSERT_TRUE(service->run().has_value());
    RunOutcome outcome = lastOutcome();
    EXPECT_TRUE(outcome.buildFailed);
    EXPECT_EQ(launcher.launches().size(), 1U) << "the program is not started after a failed build";

    launcher.expect({.expectExecutable = kMsBuild});
    launcher.expect({.expectExecutable = programPath("App"), .exitCode = 7});
    ASSERT_TRUE(service->run().has_value());
    outcome = lastOutcome();
    EXPECT_EQ(outcome.exitCode, 7) << (outcome.error ? outcome.error->describe() : "");
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 3U);
    EXPECT_EQ(launches[1].spec.executable, kMsBuild);
    EXPECT_EQ(launches[2].spec.executable, programPath("App"));
    service.reset();
    EXPECT_TRUE(buildService->waitForIdle(kWait));
}

} // namespace
} // namespace frost::build
