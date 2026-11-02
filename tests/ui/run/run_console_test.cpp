// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::ui_run, offscreen (no window is shown): RunConsole's text, tints, CR LF
// handling, input line, buttons and closing line; RunController over a real RunService (MemFs
// solution, a fake builder, the scripted launcher) with the startup combo and the stop-and-rerun
// prompt; and the run actions in an ActionRegistry.

#include "frost/build/run_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/input/action.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"

#include "build/build_test_util.hpp"
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {

using namespace std::chrono_literals;
using build::test::MemTree;
using project::ProjectId;

// Processes Qt events until `done` or 30 s.
bool spinUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// The foreground colour of the first character of `needle` in the console's output.
QColor colorOf(const RunConsole& console, const QString& needle) {
    const qsizetype at = console.text().indexOf(needle);
    if (at < 0) {
        return {};
    }
    QTextCursor cursor(console.output()->document());
    cursor.setPosition(static_cast<int>(at) + 1); // The format of the character before the position.
    return cursor.charFormat().foreground().color();
}

TEST(RunFinishedLine, ReadsLikeRider) {
    build::RunOutcome outcome;
    outcome.exitCode = 0;
    EXPECT_EQ(runFinishedLine(outcome), QStringLiteral("Process finished with exit code 0"));
    outcome.exitCode = static_cast<int>(0xC0000005U);
    EXPECT_EQ(runFinishedLine(outcome), QStringLiteral("Process finished with exit code -1073741819 (0xC0000005)"));
    outcome.exitCode = 137;
    outcome.stopped = true;
    EXPECT_EQ(runFinishedLine(outcome), QStringLiteral("Process finished with exit code 137 (stopped)"));
    build::RunOutcome failed;
    failed.buildFailed = true;
    EXPECT_EQ(runFinishedLine(failed), QStringLiteral("Build failed; the program was not started"));
    build::RunOutcome missing;
    missing.error = Error(ErrorCode::NotFound, "App.exe does not exist");
    EXPECT_EQ(runFinishedLine(missing), QStringLiteral("The program was not started: App.exe does not exist"));
}

TEST(RunConsole, ShowsTintedOutputAndTheExitCode) {
    RunConsole console;
    console.beginRun(QStringLiteral("App [Debug|x64]"));
    EXPECT_EQ(console.title()->text(), QStringLiteral("App [Debug|x64]"));
    EXPECT_TRUE(console.stopButton()->isEnabled()) << "a run in its build can be stopped";
    EXPECT_FALSE(console.input()->isEnabled());
    console.appendOutput(build::RunOutputKind::System, "\"C:\\s\\App.exe\" --flag\n");
    console.setRunning(true, true);
    EXPECT_TRUE(console.input()->isEnabled());
    // CR LF split across chunks, and a lone CR.
    console.appendOutput(build::RunOutputKind::Stdout, "hello\r");
    console.appendOutput(build::RunOutputKind::Stdout, "\nworld\r\nlast\rline");
    console.appendOutput(build::RunOutputKind::Stderr, "\noops\n");
    EXPECT_EQ(console.text(), QStringLiteral("\"C:\\s\\App.exe\" --flag\nhello\nworld\nlast\nline\noops\n"));
    const QColor out = colorOf(console, QStringLiteral("hello"));
    const QColor err = colorOf(console, QStringLiteral("oops"));
    const QColor system = colorOf(console, QStringLiteral("C:"));
    EXPECT_NE(out, err) << "stderr is tinted";
    EXPECT_NE(out, system) << "FrostIDE's lines are dimmed";

    QString submitted;
    QObject::connect(&console, &RunConsole::inputSubmitted, [&](const QString& line) { submitted = line; });
    console.input()->setText(QStringLiteral("typed"));
    emit console.input()->returnPressed();
    EXPECT_EQ(submitted, QStringLiteral("typed"));
    EXPECT_TRUE(console.input()->text().isEmpty());
    EXPECT_TRUE(console.text().endsWith(QStringLiteral("oops\ntyped\n"))) << "the typed line is echoed";

    int stops = 0;
    QObject::connect(&console, &RunConsole::stopRequested, [&] { ++stops; });
    console.stopButton()->click();
    EXPECT_EQ(stops, 1);

    build::RunOutcome outcome;
    outcome.exitCode = 3;
    console.finish(outcome);
    EXPECT_TRUE(console.text().endsWith(QStringLiteral("typed\n\nProcess finished with exit code 3\n")))
        << console.text().toStdString();
    EXPECT_FALSE(console.stopButton()->isEnabled());
    EXPECT_FALSE(console.input()->isEnabled());
    EXPECT_TRUE(console.rerunButton()->isEnabled());

    console.beginRun(QStringLiteral("App [Release|x64]"));
    EXPECT_TRUE(console.text().isEmpty()) << "a new run starts with an empty console";
}

// The build side for the controller tests: finishes builds when the test says so.
class FakeBuilder final : public build::IRunBuilder {
public:
    std::optional<std::size_t> activeConfigurationIndex() const override { return 0; }
    bool building() const override { return false; }
    Result<void> buildForRun(ProjectId, build::BuildCompletion done) override {
        LockGuard lock(m_mutex);
        m_pending.push_back(std::move(done));
        return {};
    }
    void cancelBuild() override {}

    // Finishes the oldest waiting build successfully; false when none was asked for in time.
    bool finish() {
        build::BuildCompletion done;
        if (!spinUntil([&] {
                LockGuard lock(m_mutex);
                return !m_pending.empty();
            })) {
            return false;
        }
        {
            LockGuard lock(m_mutex);
            done = std::move(m_pending.front());
            m_pending.erase(m_pending.begin());
        }
        build::BuildOutcome outcome;
        outcome.result.success = true;
        done(outcome);
        return true;
    }

private:
    Mutex m_mutex;
    std::vector<build::BuildCompletion> m_pending FROST_GUARDED_BY(m_mutex);
};

std::string vcxproj(std::string_view type) {
    return std::format(R"(<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Configuration"><ConfigurationType>{}</ConfigurationType></PropertyGroup>
</Project>
)",
                       type);
}

// Run.sln with Core (a library), App and Tool; the service runs over a MemFs and the scripted launcher.
class RunControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        constexpr std::string_view kCpp = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";
        std::string sln = "Microsoft Visual Studio Solution File, Format Version 12.00\r\n";
        std::string mappings;
        int n = 1;
        for (const std::string_view name : {"Core", "App", "Tool"}) {
            const std::string guid = std::format("{{00000000-0000-4000-8000-00000000000{}}}", n++);
            sln += std::format("Project(\"{}\") = \"{}\", \"{}\\{}.vcxproj\", \"{}\"\r\nEndProject\r\n", kCpp, name,
                               name, name, guid);
            mappings += std::format(
                "\t\t{0}.Debug|x64.ActiveCfg = Debug|x64\r\n\t\t{0}.Debug|x64.Build.0 = Debug|x64\r\n", guid);
            tree.write(std::format("Run/{}/{}.vcxproj", name, name),
                       vcxproj(name == "Core" ? "StaticLibrary" : "Application"));
            tree.write(std::format("Run/x64/Debug/{}.exe", name), build::test::peImage(3));
        }
        sln +=
            "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n\t\tDebug|x64 = Debug|x64\r\n"
            "\tEndGlobalSection\r\n\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n" +
            mappings + "\tEndGlobalSection\r\nEndGlobal\r\n";
        tree.write("Run/Run.sln", sln);
        auto opened = project::TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value());
        trustStore = std::move(*opened);
        ASSERT_TRUE(trustStore->trust(MemTree::path("Run")).has_value());
        auto loaded = project::SolutionModel::load(tree.fs, MemTree::path("Run/Run.sln"));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
        solution = std::make_shared<const project::SolutionModel>(std::move(*loaded));

        controller = std::make_unique<RunController>(
            &console, RunControllerHooks{.confirmRerun =
                                             [this](const QString& name) {
                                                 prompts.push_back(name);
                                                 return acceptRerun;
                                             },
                                         .reportError = [this](const QString& message) { errors.push_back(message); }});
        build::RunServiceOptions options{
            .stateFilePath = MemTree::path("data/run-settings.txt"),
            .properties = [](const build::RunPropertyRequest& request) -> Result<build::RunProperties> {
                std::string name(io::fileName(request.projectPath));
                name.erase(name.rfind('.'));
                build::RunProperties properties;
                properties.targetPath = MemTree::path(std::format("Run/x64/Debug/{}.exe", name));
                properties.projectDir =
                    std::string(io::parentPath(request.projectPath)) + (build::test::kWindows ? "\\" : "/");
                return properties;
            },
            .environment = [](std::string_view) -> std::optional<std::string> { return std::nullopt; },
            .msbuildRunSupported = true,
            .externalConsoleByDefault = false};
        service = build::RunService::create(tree.fs, launcher, *trustStore, builder, std::move(options),
                                            controller->callbacks());
        ASSERT_TRUE(service->openSolution(solution).has_value());
        controller->setService(service.get());
    }

    void TearDown() override {
        controller->setService(nullptr);
        service.reset();
        controller.reset();
    }

    [[nodiscard]] static std::string programPath(std::string_view name) {
        return MemTree::path(std::format("Run/x64/Debug/{}.exe", name));
    }

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    std::unique_ptr<project::TrustStore> trustStore;
    std::shared_ptr<const project::SolutionModel> solution;
    FakeBuilder builder;
    RunConsole console;
    std::vector<QString> prompts;
    std::vector<QString> errors;
    bool acceptRerun = false;
    std::unique_ptr<RunController> controller;
    std::unique_ptr<build::RunService> service;
};

TEST_F(RunControllerTest, TheStartupComboFollowsTheService) {
    QComboBox combo;
    controller->bindStartupCombo(&combo);
    ASSERT_EQ(combo.count(), 3);
    EXPECT_EQ(combo.currentText(), QStringLiteral("App"));
    EXPECT_TRUE(combo.isEnabled());

    combo.setCurrentIndex(combo.findText(QStringLiteral("Tool")));
    ASSERT_TRUE(service->startupProject().has_value());
    EXPECT_EQ(solution->project(*service->startupProject()).name, "Tool");

    // Set as Startup Project from elsewhere moves the combo.
    ASSERT_TRUE(controller->setStartupProject(ProjectId{0}).has_value());
    EXPECT_TRUE(spinUntil([&] { return combo.currentText() == QStringLiteral("Core"); }));
    EXPECT_TRUE(errors.empty());
}

TEST_F(RunControllerTest, RunsIntoTheConsoleAndPromptsBeforeARerun) {
    launcher.expect({.expectExecutable = programPath("App"), .neverExits = true});
    launcher.expect({.expectExecutable = programPath("App"), .stdoutBytes = "second run\r\n"});

    auto first = controller->run();
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    ASSERT_TRUE(builder.finish());
    ASSERT_TRUE(spinUntil([&] { return console.input()->isEnabled(); })) << console.text().toStdString();
    EXPECT_TRUE(controller->running());
    EXPECT_EQ(console.title()->text(), QStringLiteral("App [Debug|x64]"));

    // Busy: declining the prompt leaves it running.
    auto declined = controller->run();
    ASSERT_FALSE(declined.has_value());
    EXPECT_EQ(declined.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(prompts, std::vector<QString>{QStringLiteral("App")});
    EXPECT_EQ(launcher.launches().size(), 1U);

    // Accepting stops it and runs it again.
    acceptRerun = true;
    ASSERT_TRUE(controller->run().has_value());
    ASSERT_TRUE(builder.finish());
    ASSERT_TRUE(service->waitForIdle(30s)); // Every event is queued for the GUI thread now.
    EXPECT_TRUE(spinUntil([&] {
        return !controller->running() && console.text().endsWith(QStringLiteral("Process finished with exit code 0\n"));
    })) << console.text().toStdString();
    EXPECT_TRUE(console.text().contains(QStringLiteral("second run\n"))) << console.text().toStdString();
    EXPECT_FALSE(console.text().contains(QStringLiteral("(stopped)"))) << "the console shows the new run only";
    EXPECT_TRUE(launcher.launches()[0].terminated);
}

TEST_F(RunControllerTest, TheRunActionsDriveTheController) {
    input::ActionRegistry registry;
    std::optional<ProjectId> selected = ProjectId{2};
    std::vector<std::string> reported;
    ASSERT_TRUE(registerRunActions(registry, *controller,
                                   {.selectedProject = [&] { return selected; },
                                    .reportError = [&](const Error& error) { reported.push_back(error.message()); }})
                    .has_value());
    for (const std::string_view id : {kRunActionId, kRunWithoutDebuggingActionId, kStopActionId, kRerunActionId,
                                      kSetStartupProjectActionId, kToggleExternalConsoleActionId}) {
        EXPECT_TRUE(registry.find(id).has_value()) << id;
    }
    EXPECT_EQ(registerRunActions(registry, *controller).error().code(), ErrorCode::AlreadyExists);
    const input::ActionContext context{};
    const auto invoke = [&](std::string_view id) { return registry.tryInvoke(*registry.find(id), context); };

    // Set as Startup Project on Tool, then it is disabled (Tool already is).
    EXPECT_EQ(invoke(kSetStartupProjectActionId), input::InvokeResult::Ran);
    EXPECT_EQ(solution->project(*service->startupProject()).name, "Tool");
    EXPECT_FALSE(registry.isEnabled(*registry.find(kSetStartupProjectActionId), context));
    selected.reset();
    EXPECT_FALSE(registry.isEnabled(*registry.find(kSetStartupProjectActionId), context));

    EXPECT_FALSE(registry.isEnabled(*registry.find(kStopActionId), context)) << "nothing runs";
    EXPECT_EQ(invoke(kToggleExternalConsoleActionId), input::InvokeResult::Ran);
    EXPECT_TRUE(service->useExternalConsole());
    EXPECT_EQ(invoke(kToggleExternalConsoleActionId), input::InvokeResult::Ran);
    EXPECT_FALSE(service->useExternalConsole());

    // Ctrl+F5's action runs the startup project like Run; Stop then ends it.
    launcher.expect({.expectExecutable = programPath("Tool"), .neverExits = true});
    EXPECT_EQ(invoke(kRunWithoutDebuggingActionId), input::InvokeResult::Ran);
    ASSERT_TRUE(builder.finish());
    ASSERT_TRUE(spinUntil([&] { return console.input()->isEnabled(); }));
    EXPECT_TRUE(registry.isEnabled(*registry.find(kStopActionId), context));
    EXPECT_EQ(invoke(kStopActionId), input::InvokeResult::Ran);
    ASSERT_TRUE(spinUntil([&] { return !controller->running(); }));
    EXPECT_TRUE(console.text().contains(QStringLiteral("(stopped)"))) << console.text().toStdString();
    EXPECT_TRUE(reported.empty());
}

// Rider's one Stop (Ctrl+F2) also ends a debug session (RunActionHooks::debugging, stopDebugging).
TEST_F(RunControllerTest, StopAlsoEndsADebugSession) {
    input::ActionRegistry registry;
    bool debugging = false;
    int debugStops = 0;
    ASSERT_TRUE(registerRunActions(registry, *controller,
                                   {.debugging = [&] { return debugging; }, .stopDebugging = [&] { ++debugStops; }})
                    .has_value());
    const input::ActionContext context{};
    EXPECT_FALSE(registry.isEnabled(*registry.find(kStopActionId), context));
    debugging = true;
    EXPECT_TRUE(registry.isEnabled(*registry.find(kStopActionId), context)) << "a debug session can be stopped";
    EXPECT_EQ(registry.tryInvoke(*registry.find(kStopActionId), context), input::InvokeResult::Ran);
    EXPECT_EQ(debugStops, 1);
}

// A Debug run (RunService::debug) belongs to the Debug tool window: the Run console stays as it was.
TEST_F(RunControllerTest, ADebugRunIsLeftToTheDebugger) {
    std::optional<build::DebugLaunch> handed; // Written before `finished` is set (the service's thread).
    std::atomic<bool> finished = false;
    auto started = service->debug(
        std::nullopt,
        {.onOutput = {}, .onFinished = [&](const build::RunOutcome&, const std::optional<build::DebugLaunch>& launch) {
             handed = launch;
             finished = true;
         }});
    ASSERT_TRUE(started.has_value()) << started.error().describe();
    ASSERT_TRUE(builder.finish());
    ASSERT_TRUE(spinUntil([&] { return finished.load(); }));
    ASSERT_TRUE(handed.has_value());
    EXPECT_EQ(handed->command.executable, programPath("App"));
    EXPECT_FALSE(controller->running());
    EXPECT_TRUE(console.text().isEmpty()) << console.text().toStdString();
    EXPECT_TRUE(launcher.launches().empty());
}

} // namespace
} // namespace frost::ui
