// SPDX-License-Identifier: Apache-2.0
//
// frost::run unit tests (label unit): RunConfiguration validation, the JSON store's reader and
// writer, load and save over a MemFs workspace, launchPlanFor and displayCommandLine, and the
// RunManager rules that start no process (refusals, Busy, Prepare, a failing before-launch step).

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "run/run_collector.hpp"
#include <gtest/gtest.h>

#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace frost::run {
namespace {

using namespace std::chrono_literals;

// An absolute path in the native style: "/work/<rest>" on POSIX, "C:\work\<rest>" on Windows.
std::string native(std::string_view rest) {
    std::string path = io::kNativePathStyle == io::PathStyle::Windows ? "C:\\work" : "/work";
    if (!rest.empty()) {
        path += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
        for (const char c : rest) {
            path += (c == '/' && io::kNativePathStyle == io::PathStyle::Windows) ? '\\' : c;
        }
    }
    return path;
}

RunConfiguration nativeConfiguration(std::string id = "app") {
    RunConfiguration configuration;
    configuration.id = std::move(id);
    configuration.name = "App";
    configuration.type = RunType::Native;
    configuration.program = native("bin/app.exe");
    return configuration;
}

RunConfiguration everyField() {
    RunConfiguration configuration;
    configuration.id = "frost-editor.Debug_1";
    configuration.name = "FrostEditor \"debug\" \xC3\xA9";
    configuration.type = RunType::CMakeTarget;
    configuration.target = "FrostEditor";
    configuration.program = native("build/FrostEditor.exe");
    configuration.arguments = {"--project", "Demo Game", "", "quote\"back\\slash"};
    configuration.workingDirectory = native("games/demo");
    configuration.environment = {{"FROST_LOG", "debug"}, {"TMP", std::nullopt}, {"EMPTY", ""}};
    configuration.beforeLaunch = BeforeLaunch::Build;
    return configuration;
}

TEST(RunConfiguration, TypeAndStepSpellingsRoundTrip) {
    for (const RunType type : {RunType::Native, RunType::CMakeTarget, RunType::MSBuild}) {
        EXPECT_EQ(parseRunType(toString(type)), type);
    }
    EXPECT_EQ(toString(RunType::CMakeTarget), "cmake-target");
    EXPECT_EQ(toString(RunType::MSBuild), "msbuild");
    EXPECT_EQ(toString(RunType::Native), "native");
    EXPECT_FALSE(parseRunType("CMake-Target").has_value());
    EXPECT_EQ(parseBeforeLaunch("build"), BeforeLaunch::Build);
    EXPECT_EQ(parseBeforeLaunch("none"), BeforeLaunch::None);
    EXPECT_FALSE(parseBeforeLaunch("Build").has_value());
}

TEST(RunConfiguration, ValidationNamesWhatIsWrong) {
    EXPECT_TRUE(validateRunConfiguration(nativeConfiguration()).has_value());
    EXPECT_TRUE(validateRunConfiguration(everyField()).has_value());

    const auto refused = [](RunConfiguration configuration, std::string_view fragment) {
        auto checked = validateRunConfiguration(configuration);
        ASSERT_FALSE(checked.has_value()) << fragment;
        EXPECT_EQ(checked.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(checked.error().message().find(fragment), std::string::npos) << checked.error().message();
    };
    auto c = nativeConfiguration("bad id");
    refused(c, "the id");
    c = nativeConfiguration(std::string(129, 'a'));
    refused(c, "the id");
    c = nativeConfiguration("");
    refused(c, "the id");
    c = nativeConfiguration();
    c.name.clear();
    refused(c, "the name");
    c = nativeConfiguration();
    c.program.clear();
    refused(c, "needs a program");
    c = nativeConfiguration();
    c.target = "x";
    refused(c, "no target");
    c = nativeConfiguration();
    c.beforeLaunch = BeforeLaunch::Build;
    refused(c, "nothing to build");
    c = nativeConfiguration();
    c.program = "bin/app.exe";
    refused(c, "not an absolute path");
    c = nativeConfiguration();
    c.workingDirectory = "relative";
    refused(c, "working directory");
    c = nativeConfiguration();
    c.arguments = {std::string("a\0b", 3)};
    refused(c, "argument contains NUL");
    c = nativeConfiguration();
    c.environment = {{"A=B", "1"}};
    refused(c, "environment name");
    c = nativeConfiguration();
    c.environment = {{"", "1"}};
    refused(c, "environment name");

    RunConfiguration cmake;
    cmake.id = "t";
    cmake.name = "T";
    cmake.type = RunType::CMakeTarget;
    refused(cmake, "target name");
    cmake.target = "Game";
    EXPECT_TRUE(validateRunConfiguration(cmake).has_value()) << "the program may be left to the before-launch step";

    RunConfiguration msbuild = cmake;
    msbuild.type = RunType::MSBuild;
    msbuild.target = "Game.vcxproj";
    refused(msbuild, "absolute path of its project file");
    msbuild.target = native("Game/Game.vcxproj");
    EXPECT_TRUE(validateRunConfiguration(msbuild).has_value());
}

TEST(RunConfigurationStore, SerializedConfigurationsReadBackEqual) {
    const std::vector<RunConfiguration> configurations{everyField(), nativeConfiguration()};
    const std::string text = serializeRunConfigurations(configurations);
    EXPECT_TRUE(text.ends_with("\n"));
    EXPECT_NE(text.find("\"version\": 1"), std::string::npos);
    EXPECT_NE(text.find("\"type\": \"cmake-target\""), std::string::npos);
    EXPECT_NE(text.find("{\"name\":\"TMP\",\"unset\":true}"), std::string::npos);
    auto parsed = parseRunConfigurations(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(*parsed, configurations);

    auto none = parseRunConfigurations(serializeRunConfigurations({}));
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
}

// The JSON spelling of a native path (backslashes escaped on Windows).
std::string quoted(std::string_view rest) { return rpc::Json(native(rest)).dump(); }

TEST(RunConfigurationStore, ReadsMinimalEntriesAndIgnoresUnknownMembers) {
    const std::string text = "\xEF\xBB\xBF{\"version\": 1, \"future\": {\"x\": 1}, \"configurations\": ["
                             "{\"id\": \"a\", \"name\": \"A\", \"type\": \"native\", \"program\": " +
                             quoted("a.exe") +
                             ", \"color\": \"blue\"},"
                             "{\"id\": \"b\", \"name\": \"B\", \"type\": \"cmake-target\", \"target\": \"Game\","
                             " \"beforeLaunch\": \"build\"}]}";
    auto parsed = parseRunConfigurations(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    ASSERT_EQ(parsed->size(), 2U);
    EXPECT_EQ((*parsed)[0].program, native("a.exe"));
    EXPECT_TRUE((*parsed)[0].arguments.empty());
    EXPECT_EQ((*parsed)[0].beforeLaunch, BeforeLaunch::None);
    EXPECT_EQ((*parsed)[1].type, RunType::CMakeTarget);
    EXPECT_EQ((*parsed)[1].target, "Game");
    EXPECT_TRUE((*parsed)[1].program.empty());
    EXPECT_EQ((*parsed)[1].beforeLaunch, BeforeLaunch::Build);

    auto noList = parseRunConfigurations("{\"version\": 1}");
    ASSERT_TRUE(noList.has_value());
    EXPECT_TRUE(noList->empty());
}

TEST(RunConfigurationStore, RefusesDamagedFilesWhole) {
    const auto refused = [](const std::string& text, ErrorCode code, std::string_view fragment) {
        auto parsed = parseRunConfigurations(text);
        ASSERT_FALSE(parsed.has_value()) << text;
        EXPECT_EQ(parsed.error().code(), code) << parsed.error().describe();
        EXPECT_NE(parsed.error().message().find(fragment), std::string::npos) << parsed.error().message();
    };
    const std::string good = "{\"id\": \"a\", \"name\": \"A\", \"type\": \"native\", \"program\": " + quoted("a.exe");
    const std::string head = "{\"version\": 1, \"configurations\": [";
    refused("not json", ErrorCode::Corrupt, "run configurations");
    refused("[]", ErrorCode::Corrupt, "not a JSON object");
    refused("{\"configurations\": []}", ErrorCode::Corrupt, "'version'");
    refused("{\"version\": 0}", ErrorCode::Corrupt, "version 0");
    refused("{\"version\": 2, \"configurations\": []}", ErrorCode::Unsupported, "version 2 is newer");
    refused("{\"version\": 1, \"configurations\": {}}", ErrorCode::Corrupt, "not an array");
    refused(head + good + "}, " + good + "}]}", ErrorCode::Corrupt, "#2: the id 'a' is used twice");
    refused(head + good + "}, 3]}", ErrorCode::Corrupt, "#2: not an object");
    refused(head + "{\"id\": \"a\", \"type\": \"native\"}]}", ErrorCode::Corrupt, "'name' is missing");
    refused(head + good + ", \"type\": \"python\"}]}", ErrorCode::Corrupt, "unknown type 'python'");
    refused(head + good + ", \"beforeLaunch\": \"deploy\"}]}", ErrorCode::Corrupt, "unknown beforeLaunch 'deploy'");
    refused(head + good + ", \"arguments\": \"-v\"}]}", ErrorCode::Corrupt, "'arguments' is not an array");
    refused(head + good + ", \"arguments\": [1]}]}", ErrorCode::Corrupt, "an argument is not a string");
    refused(head + good + ", \"program\": 5}]}", ErrorCode::Corrupt, "'program' is not a string");
    refused(head + good + ", \"environment\": [{\"name\": \"X\", \"value\": \"1\", \"unset\": true}]}]}",
            ErrorCode::Corrupt, "both a value and unset");
    refused(head + good + ", \"environment\": [{\"name\": \"X\"}]}]}", ErrorCode::Corrupt, "neither a value nor unset");
    refused(head + good + ", \"program\": \"relative.exe\"}]}", ErrorCode::Corrupt, "not an absolute path");
}

TEST(RunConfigurationStore, SavesUnderTheWorkspacesFrostFolderAndLoadsBack) {
    ManualClock clock;
    io::MemFs fs(clock);
    const std::string root = native("game");
    ASSERT_TRUE(fs.createDirectories(root).has_value());
    auto path = workspaceRunConfigurationsPath(root);
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, native("game/.frost/runConfigurations/configurations.json"));
    EXPECT_FALSE(workspaceRunConfigurationsPath("relative").has_value());

    auto empty = loadRunConfigurations(fs, *path);
    ASSERT_TRUE(empty.has_value()) << "a workspace without the file has no configurations";
    EXPECT_TRUE(empty->empty());
    EXPECT_FALSE(fs.exists(native("game/.frost")).value()) << "loading creates nothing";

    // A refused save writes nothing, not even .frost/.
    RunConfiguration broken = nativeConfiguration();
    broken.program.clear();
    const std::vector<RunConfiguration> refused{broken};
    EXPECT_EQ(saveWorkspaceRunConfigurations(fs, root, refused).error().code(), ErrorCode::InvalidArgument);
    const std::vector<RunConfiguration> twins{nativeConfiguration("x"), nativeConfiguration("x")};
    EXPECT_EQ(saveWorkspaceRunConfigurations(fs, root, twins).error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs.exists(native("game/.frost")).value());

    const std::vector<RunConfiguration> configurations{everyField(), nativeConfiguration()};
    ASSERT_TRUE(saveWorkspaceRunConfigurations(fs, root, configurations).has_value());
    EXPECT_TRUE(fs.exists(native("game/.frost/.gitignore")).value()) << "the .frost layout is written first";
    auto loaded = loadRunConfigurations(fs, *path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    EXPECT_EQ(*loaded, configurations);
    EXPECT_EQ(fs.readFile(*path).value(), serializeRunConfigurations(configurations));

    // A damaged file is reported with its path and left alone.
    ASSERT_TRUE(fs.writeFileAtomic(*path, "{\"version\": 1, \"configurations\": [7]}").has_value());
    auto damaged = loadRunConfigurations(fs, *path);
    ASSERT_FALSE(damaged.has_value());
    EXPECT_EQ(damaged.error().code(), ErrorCode::Corrupt);
    EXPECT_NE(damaged.error().describe().find("configurations.json"), std::string::npos) << damaged.error().describe();
}

TEST(RunLaunchPlan, UsesTheProgramsFolderWithoutAWorkingDirectory) {
    RunConfiguration configuration = nativeConfiguration();
    configuration.program = native("bin/./tools/../app.exe");
    configuration.arguments = {"a b"};
    configuration.environment = {{"K", "V"}};
    auto plan = launchPlanFor(configuration);
    ASSERT_TRUE(plan.has_value()) << plan.error().describe();
#ifdef _WIN32
    EXPECT_EQ(plan->program, native("bin/app.exe"));
    EXPECT_EQ(plan->workingDirectory, native("bin"));
#else
    /* POSIX normalisation drops "." but keeps "..": "tools/.." is the parent of wherever the tools
       symlink points, not lexically "bin". The kernel resolves the program and the folder the same
       way, so the folder is still the one that holds the program. */
    EXPECT_EQ(plan->program, native("bin/tools/../app.exe"));
    EXPECT_EQ(plan->workingDirectory, native("bin/tools/.."));
#endif
    EXPECT_EQ(plan->arguments, configuration.arguments);
    EXPECT_EQ(plan->environment, configuration.environment);

    configuration.workingDirectory = native("data");
    EXPECT_EQ(launchPlanFor(configuration)->workingDirectory, native("data"));

    RunConfiguration unresolved;
    unresolved.id = "t";
    unresolved.name = "T";
    unresolved.type = RunType::CMakeTarget;
    unresolved.target = "Game";
    EXPECT_EQ(launchPlanFor(unresolved).error().code(), ErrorCode::InvalidArgument);
}

TEST(RunLaunchPlan, DisplayCommandLineQuotesWhatNeedsQuoting) {
    LaunchPlan plan;
    plan.program = native("my tools/app.exe");
    plan.arguments = {"plain", "two words", ""};
    if constexpr (io::kWindowsProcessRules) {
        EXPECT_EQ(displayCommandLine(plan), "\"C:\\work\\my tools\\app.exe\" plain \"two words\" \"\"");
    } else {
        EXPECT_EQ(displayCommandLine(plan), "'/work/my tools/app.exe' plain 'two words' ''");
        plan.arguments = {"it's"};
        EXPECT_EQ(displayCommandLine(plan), "'/work/my tools/app.exe' 'it'\\''s'");
    }
}

// ---- RunManager rules that start no process.

RunConfiguration cmakeConfiguration() {
    RunConfiguration configuration;
    configuration.id = "game";
    configuration.name = "Game";
    configuration.type = RunType::CMakeTarget;
    configuration.target = "Game";
    return configuration;
}

TEST(RunManagerRules, RefusesInvalidConfigurationsAndMissingBeforeLaunchHandlers) {
    auto manager = RunManager::create(io::realProcessLauncher());
    RunConfiguration broken = nativeConfiguration();
    broken.program = "relative.exe";
    EXPECT_EQ(manager->start(broken).error().code(), ErrorCode::InvalidArgument);

    RunConfiguration cmake = cmakeConfiguration();
    auto refused = manager->start(cmake);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(refused.error().message().find("RUN-2"), std::string::npos) << refused.error().message();
    cmake.program = native("bin/game.exe");
    cmake.beforeLaunch = BeforeLaunch::Build;
    EXPECT_EQ(manager->start(cmake).error().code(), ErrorCode::Unsupported) << "a Build step needs the handler too";
    EXPECT_TRUE(manager->activeRuns().empty());
    EXPECT_TRUE(manager->waitForIdle(5s));
}

TEST(RunManagerRules, PrepareResolvesThePlanAndStartsNothing) {
    std::vector<std::string> asked;
    std::vector<std::jthread> workers;
    RunManagerOptions options;
    // The handler answers on another thread, as a build would.
    options.beforeLaunch = [&](const RunConfiguration& configuration, std::stop_token, BeforeLaunchDone done) {
        asked.push_back(configuration.target);
        workers.emplace_back(
            [done = std::move(done)] { done(LaunchPlan{.program = native("build/./game.exe"), .arguments = {"-x"}}); });
    };
    std::vector<std::string> listened;
    options.listener.onStarted = [&](const RunStatus& status) {
        listened.push_back("started:" + status.configuration.id);
        EXPECT_EQ(status.phase, RunPhase::BeforeLaunch);
        EXPECT_EQ(status.mode, RunMode::Prepare);
    };
    options.listener.onFinished = [&](const RunResult&) { listened.emplace_back("finished"); };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));

    RunConfiguration cmake = cmakeConfiguration();
    cmake.beforeLaunch = BeforeLaunch::Build;
    test::RunCollector collector;
    auto id = manager->start(cmake, collector.handlers(), RunMode::Prepare);
    ASSERT_TRUE(id.has_value()) << id.error().describe();
    EXPECT_EQ(manager->start(cmake).error().code(), ErrorCode::Busy) << "one run per configuration";
    const auto finished = collector.waitFinished();
    ASSERT_TRUE(finished.result.has_value());
    const RunResult& result = *finished.result;
    EXPECT_EQ(result.id, *id);
    EXPECT_EQ(result.configurationId, "game");
    EXPECT_EQ(result.mode, RunMode::Prepare);
    EXPECT_FALSE(result.ran());
    EXPECT_FALSE(result.error.has_value());
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->program, native("build/game.exe"));
    EXPECT_EQ(result.plan->workingDirectory, native("build")) << "the program's folder";
    EXPECT_EQ(result.plan->arguments, std::vector<std::string>{"-x"});
    EXPECT_EQ(finished.events, std::vector<std::string>{"finished"}) << "nothing ran, so no phase or output";
    ASSERT_TRUE(manager->waitForIdle(10s));
    EXPECT_EQ(asked, std::vector<std::string>{"Game"});
    EXPECT_EQ(listened, (std::vector<std::string>{"started:game", "finished"}));
    EXPECT_FALSE(manager->activeRun("game").has_value());
    workers.clear();
}

TEST(RunManagerRules, AFailedBeforeLaunchStepEndsTheRunWithItsError) {
    RunManagerOptions options;
    options.beforeLaunch = [](const RunConfiguration&, std::stop_token, BeforeLaunchDone done) {
        done(fail(ErrorCode::IoError, "the build failed: 2 errors")); // Inside the call is allowed too.
    };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));
    test::RunCollector collector;
    ASSERT_TRUE(manager->start(cmakeConfiguration(), collector.handlers()).has_value());
    const auto finished = collector.waitFinished();
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_TRUE(finished.result->beforeLaunchFailed);
    EXPECT_FALSE(finished.result->ran());
    EXPECT_FALSE(finished.result->stopped);
    ASSERT_TRUE(finished.result->error.has_value());
    EXPECT_EQ(finished.result->error->message(), "the build failed: 2 errors");

    // A plan that is not absolute is refused, not launched.
    RunManagerOptions relative;
    relative.beforeLaunch = [](const RunConfiguration&, std::stop_token, BeforeLaunchDone done) {
        done(LaunchPlan{.program = "game.exe"});
    };
    auto second = RunManager::create(io::realProcessLauncher(), std::move(relative));
    test::RunCollector refused;
    ASSERT_TRUE(second->start(cmakeConfiguration(), refused.handlers()).has_value());
    const auto ended = refused.waitFinished();
    ASSERT_TRUE(ended.result && ended.result->error);
    EXPECT_EQ(ended.result->error->code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(ended.result->ran());
}

TEST(RunManagerRules, StopCancelsTheBeforeLaunchStep) {
    using StopCallback = std::stop_callback<std::function<void()>>;
    std::unique_ptr<StopCallback> cancel; // Written on the dispatcher thread before `called` is set.
    std::promise<void> called;
    RunManagerOptions options;
    options.beforeLaunch = [&](const RunConfiguration&, std::stop_token stop, BeforeLaunchDone done) {
        // A build that only ends when it is cancelled.
        cancel = std::make_unique<StopCallback>(
            stop, std::function<void()>([done] { done(fail(ErrorCode::Cancelled, "build cancelled")); }));
        called.set_value();
    };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));
    RunConfiguration cmake = cmakeConfiguration();
    cmake.beforeLaunch = BeforeLaunch::Build;
    test::RunCollector collector;
    auto id = manager->start(cmake, collector.handlers());
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(called.get_future().wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(manager->status(*id).has_value());
    EXPECT_EQ(manager->status(*id)->phase, RunPhase::BeforeLaunch);
    manager->stop(*id);
    const auto finished = collector.waitFinished();
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_TRUE(finished.result->stopped);
    EXPECT_FALSE(finished.result->beforeLaunchFailed) << "a stop is not a failure";
    EXPECT_FALSE(finished.result->ran());
    ASSERT_TRUE(manager->waitForIdle(10s));
    manager.reset();
    cancel.reset();
}

} // namespace
} // namespace frost::run
