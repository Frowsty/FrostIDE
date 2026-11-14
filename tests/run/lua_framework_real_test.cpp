// SPDX-License-Identifier: Apache-2.0
// Future real LUA-12 acceptance: external supported modules are required, never skipped.
// Discovery is compared with independent genuine framework runs, not expected parser specimens.
#include "frost/debug/standalone_lua_dap.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/file_system.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/test_runner.hpp"

#include "debug/debug_test_util.hpp"
#include "io/process_env.hpp"
#include "support/scratch_dir.hpp"
#include <pugixml.hpp>

#include <cstdlib>
#include <future>
#include <map>
namespace {
using namespace frost;
using namespace frost::run;
using namespace std::chrono_literals;
RunConfiguration configuration(std::string target) {
    RunConfiguration c;
    c.id = "framework-real";
    c.name = "Lua framework";
    c.type = RunType::LuaScript;
    c.program = FROST_TEST_LUA_INTERPRETER;
    c.target = std::move(target);
    c.workingDirectory = FROST_LUA_FRAMEWORK_FIXTURES;
    if (const auto* path = std::getenv("FROST_LUA_TEST_PATH"))
        c.environment.push_back({"LUA_PATH", path});
    if (const auto* path = std::getenv("FROST_LUA_TEST_CPATH"))
        c.environment.push_back({"LUA_CPATH", path});
    return c;
}
Result<std::vector<FrameworkCase>> discover(TestRunner& runner, TestRunner::LuaFrameworkRequest request) {
    auto promise = std::make_shared<std::promise<Result<std::vector<FrameworkCase>>>>();
    auto future = promise->get_future();
    auto started =
        runner.listLuaFramework(std::move(request), [promise](auto value) { promise->set_value(std::move(value)); });
    if (!started)
        return std::unexpected(started.error());
    if (future.wait_for(20s) != std::future_status::ready) {
        runner.cancel();
        return fail(ErrorCode::Timeout, "real Lua discovery did not complete");
    }
    return future.get();
}
Result<TestRunReport> execute(TestRunner& runner, TestRunner::LuaFrameworkRequest request) {
    auto promise = std::make_shared<std::promise<TestRunReport>>();
    auto future = promise->get_future();
    TestRunHandlers h;
    h.onFinished = [promise](auto value) { promise->set_value(std::move(value)); };
    auto started = runner.runLuaFramework(std::move(request), std::move(h));
    if (!started)
        return std::unexpected(started.error());
    if (future.wait_for(20s) != std::future_status::ready) {
        runner.cancel();
        return fail(ErrorCode::Timeout, "real Lua framework did not complete");
    }
    return future.get();
}
std::map<std::string, TestOutcome> genuineCases(const std::string& xml) {
    // Independent read of upstream verdict nodes, not the product parser under test.
    pugi::xml_document doc;
    EXPECT_TRUE(doc.load_string(xml.c_str()));
    std::map<std::string, TestOutcome> out;
    for (auto c : doc.child("testsuites").child("testsuite").children("testcase")) {
        const std::string name = c.attribute("name").as_string();
        EXPECT_FALSE(out.contains(name));
        out[name] = c.child("failure") || c.child("error") ? TestOutcome::Failed
                    : c.child("skipped")                   ? TestOutcome::Skipped
                                                           : TestOutcome::Passed;
    }
    return out;
}
TEST(LuaFrameworkReal, DiscoveryAndResultsEqualActualInstalledFrameworksAndLiteralSelection) {
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_INTERPRETER));
    ASSERT_TRUE(io::realFileSystem().status(FROST_TEST_LUA_INTERPRETER));
    ASSERT_TRUE(std::getenv("FROST_LUA_TEST_PATH"))
        << "Provision Busted 2.2.0/LuaUnit 3.5 for this interpreter and set FROST_LUA_TEST_PATH";
    auto& fs = io::realFileSystem();
    frost::test::ScratchDir scratch("lua-framework-real");
    auto runner = TestRunner::create(io::realProcessLauncher(), fs);
    for (auto profile : {LuaTestProfile::Busted220, LuaTestProfile::LuaUnit35}) {
        auto c = configuration(profile == LuaTestProfile::Busted220
                                   ? FROST_TEST_BUSTED_ENTRY
                                   : std::string(FROST_LUA_FRAMEWORK_FIXTURES) + "/luaunit_driver.lua");
        if (profile == LuaTestProfile::Busted220) {
            ASSERT_TRUE(io::isAbsolutePath(c.target));
            ASSERT_TRUE(fs.status(c.target));
            c.arguments = {std::string(FROST_LUA_FRAMEWORK_FIXTURES) + "/busted_spec.lua"};
        }
        const auto marker = scratch.join("body-marker");
        static_cast<void>(fs.remove(marker));
        c.environment.push_back({"FROST_LUA_TEST_BODY_MARKER", marker});
        TestRunner::LuaFrameworkRequest request{c, profile, scratch.path(), FROST_LUA_DISCOVERY_BRIDGE, {}};
        auto listed = discover(*runner, request);
        ASSERT_TRUE(listed) << listed.error().describe();
        ASSERT_FALSE(listed->empty());
        EXPECT_FALSE(fs.exists(marker).value_or(true)) << "discovery executed a test body";
        // A separate, unbridged genuine run is the selection oracle (LuaUnit has no list CLI).
        auto oracle = c;
        const auto path = scratch.join("oracle.xml");
        if (profile == LuaTestProfile::Busted220)
            oracle.arguments.insert(oracle.arguments.end(),
                                    {"--ignore-lua", "--repeat=1", "--output=junit", "-Xoutput", path});
        else
            oracle.arguments.insert(oracle.arguments.end(), {"--output", "junit", "--name", path});
        auto plan = launchPlanFor(oracle);
        ASSERT_TRUE(plan);
        io::ProcessSpec spec;
        spec.executable = plan->program;
        spec.arguments = plan->arguments;
        spec.workingDirectory = plan->workingDirectory;
        spec.killOnClose = true;
        for (const auto& e : plan->environment)
            spec.environment.emplace_back(e.name, e.value);
        auto direct = io::runToCompletion(io::realProcessLauncher(), spec, "", 20s);
        ASSERT_TRUE(direct);
        EXPECT_NE(direct->exitCode, 0);
        EXPECT_TRUE(fs.exists(marker).value_or(false)) << "independent normal run did not execute bodies";
        auto bytes = fs.readFile(path);
        ASSERT_TRUE(bytes);
        auto expected = genuineCases(*bytes);
        ASSERT_FALSE(expected.empty());
        EXPECT_EQ(listed->size(), expected.size());
        for (const auto& item : *listed)
            EXPECT_TRUE(expected.contains(item.name));
        auto all = execute(*runner, request);
        ASSERT_TRUE(all);
        ASSERT_FALSE(all->error) << all->error->describe();
        ASSERT_EQ(all->results.size(), 1u);
        EXPECT_EQ(all->results[0].cases.size(), expected.size());
        for (const auto& result : all->results[0].cases) {
            ASSERT_TRUE(expected.contains(result.name));
            EXPECT_EQ(result.outcome, expected.at(result.name));
        }
        request.cases = {profile == LuaTestProfile::Busted220 ? "selection literal[x]%-?"
                                                              : "TestSelection.testLiteral[x]%-?"};
        auto selected = execute(*runner, request);
        ASSERT_TRUE(selected);
        ASSERT_FALSE(selected->error) << selected->error->describe();
        ASSERT_EQ(selected->results.size(), 1u);
        ASSERT_EQ(selected->results[0].cases.size(), 1u);
        EXPECT_EQ(selected->results[0].cases[0].name, request.cases[0]);
        EXPECT_EQ(selected->results[0].cases[0].outcome, TestOutcome::Passed);
    }
}
TEST(LuaFrameworkReal, UnobservedDriverHookAndMissingModulesAreErrorsNotEmptySuccess) {
    ASSERT_TRUE(std::getenv("FROST_LUA_TEST_PATH"));
    frost::test::ScratchDir scratch("lua-framework-unsupported");
    auto& fs = io::realFileSystem();
    const auto driver = scratch.join("not-a-runner.lua");
    ASSERT_TRUE(fs.writeFileAtomic(driver, "print('driver never invokes LuaUnit')\n"));
    auto c = configuration(driver);
    auto runner = TestRunner::create(io::realProcessLauncher(), fs);
    TestRunner::LuaFrameworkRequest request{
        c, LuaTestProfile::LuaUnit35, scratch.path(), FROST_LUA_DISCOVERY_BRIDGE, {}};
    auto absent = discover(*runner, request);
    EXPECT_FALSE(absent);
    request.configuration.environment = {{"LUA_PATH", scratch.join("missing/?.lua")},
                                         {"LUA_CPATH", scratch.join("missing/?.so")}};
    auto missing = discover(*runner, request);
    EXPECT_FALSE(missing);
}
class ObservedLauncher final : public io::IProcessLauncher {
public:
    std::promise<io::ProcessId> launched;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto result = io::realProcessLauncher().launch(spec);
        if (result)
            launched.set_value((*result)->pid());
        return result;
    }
};
TEST(LuaFrameworkReal, CancellationEndsActualRunningChildAndReportsNoInventedResult) {
    ASSERT_TRUE(std::getenv("FROST_LUA_TEST_PATH"));
    auto config = configuration(std::string(FROST_LUA_FRAMEWORK_FIXTURES) + "/luaunit_blocking.lua");
    frost::test::ScratchDir scratch("lua-framework-cancel");
    ObservedLauncher launcher;
    auto launched = launcher.launched.get_future();
    auto runner = TestRunner::create(launcher, io::realFileSystem());
    struct Capture {
        std::promise<void> ready;
        std::promise<TestRunReport> finished;
        std::string output;
        bool signalled = false;
    };
    auto capture = std::make_shared<Capture>();
    auto body = capture->ready.get_future();
    auto report = capture->finished.get_future();
    TestRunHandlers h;
    h.onOutput = [capture](std::string_view chunk) {
        capture->output += chunk;
        if (!capture->signalled && capture->output.find("LUA_TEST_BODY_RUNNING") != capture->output.npos) {
            capture->signalled = true;
            capture->ready.set_value();
        }
    };
    h.onFinished = [capture](const auto& value) { capture->finished.set_value(value); };
    ASSERT_TRUE(runner->runLuaFramework(
        {config, LuaTestProfile::LuaUnit35, scratch.path(), FROST_LUA_DISCOVERY_BRIDGE, {}}, std::move(h)));
    ASSERT_EQ(launched.wait_for(20s), std::future_status::ready);
    frost::test::ProcessWatch child(launched.get());
    ASSERT_TRUE(child.valid());
    ASSERT_EQ(body.wait_for(20s), std::future_status::ready);
    runner->cancel();
    ASSERT_EQ(report.wait_for(20s), std::future_status::ready);
    const auto final = report.get();
    EXPECT_TRUE(final.stopped);
    EXPECT_TRUE(final.results.empty());
    EXPECT_TRUE(child.exitsWithin(20s));
}
TEST(LuaFrameworkReal, LuaUnitSelectedCaseDebugUsesSameInterpreterAndRealStoppedFrame) {
    ASSERT_TRUE(std::getenv("FROST_LUA_TEST_PATH"));
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_DEBUG_ADAPTER));
    auto c = configuration(std::string(FROST_LUA_FRAMEWORK_FIXTURES) + "/luaunit_driver.lua");
    auto selected = luaFrameworkConfiguration(LuaTestProfile::LuaUnit35, c,
                                              std::vector<std::string>{"TestSelection.testPass"}, {}, false, {});
    ASSERT_TRUE(selected);
    debug::StandaloneLuaTarget target;
    target.interpreter = selected->program;
    target.script = selected->target;
    target.workingDirectory = selected->workingDirectory;
    target.interpreterArguments = selected->interpreterArguments;
    target.arguments = selected->arguments;
    target.stopOnEntry = false;
    for (const auto& e : selected->environment)
        if (e.value)
            target.environment.emplace_back(e.name, *e.value);
    auto launch = debug::standaloneLuaLaunchArguments(target);
    ASSERT_TRUE(launch);
    EXPECT_EQ(launch->stringAt("luaexe"), c.program);
    auto source = io::realFileSystem().readFile(c.target);
    ASSERT_TRUE(source);
    auto marker = source->find("LUA_FRAMEWORK_BREAKPOINT");
    ASSERT_NE(marker, std::string::npos);
    const auto line = 1 + std::count(source->begin(), source->begin() + static_cast<std::ptrdiff_t>(marker), '\n');
    debug::test::SessionRecorder recorder;
    auto session = debug::DebugSession::start(
        debug::standaloneLuaAdapterFactory(io::realProcessLauncher(), FROST_TEST_LUA_DEBUG_ADAPTER), std::move(*launch),
        {{c.target, {{.id = 1, .line = line}}}}, recorder.callbacks(), false, "lua");
    ASSERT_TRUE(session);
    ASSERT_TRUE(debug::test::waitUntil([&] { return (*session)->selectedFrame().has_value(); }));
    EXPECT_EQ((*session)->selectedFrame()->line, line);
    ASSERT_TRUE((*session)->continueRun());
    ASSERT_TRUE(debug::test::waitUntil([&] { return recorder.hasEnded(); }));
    ASSERT_TRUE((*session)->outcome());
    EXPECT_EQ((*session)->outcome()->exitCode, 0);
}
} // namespace
