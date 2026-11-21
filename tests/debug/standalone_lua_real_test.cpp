// SPDX-License-Identifier: Apache-2.0
// Actual installed interpreter/adapter workflows. No engine, fake peer, skipped tool or synthetic REPL.
// These workflows do not relabel engine-only owned-Play scenarios as standalone conformance passes.
#include "frost/debug/standalone_lua_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/run/run_manager.hpp"

#include "debug_test_util.hpp"
#include "run/run_collector.hpp"
#include "support/scratch_dir.hpp"

#include <algorithm>
#include <future>
namespace frost::debug {
namespace {
using namespace std::chrono_literals;
template<class T, class Send>
Result<T> answer(Send send) {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    send([promise](Result<T> value) { promise->set_value(std::move(value)); });
    if (future.wait_for(20s) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "standalone DAP response did not complete");
    return future.get();
}
std::int64_t marker(std::string_view source, std::string_view name) {
    const auto pos = source.find(name);
    EXPECT_NE(pos, std::string::npos);
    const auto prefix = pos == std::string::npos ? std::string_view{} : source.substr(0, pos);
    return 1 + std::count(prefix.begin(), prefix.end(), '\n');
}
run::RunConfiguration scriptConfiguration(const std::string& directory) {
    run::RunConfiguration config;
    config.id = "lua.real";
    config.name = "Standalone Lua";
    config.type = run::RunType::LuaScript;
    config.program = FROST_TEST_LUA_INTERPRETER;
    config.target = FROST_TEST_LUA_SOURCE;
    config.workingDirectory = directory;
    config.arguments = {"two words", ""};
    return config;
}
TEST(StandaloneLuaReal, PinnedClangInterpreterRunsScriptAndActualInputEvaluatesInRepl) {
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_INTERPRETER)) << "build/configure the source-pinned Lua runtime";
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.status(FROST_TEST_LUA_INTERPRETER));
    io::ProcessSpec version;
    version.executable = FROST_TEST_LUA_INTERPRETER;
    version.arguments = {"-v"};
    version.stdoutMode = io::Stdio::Pipe;
    version.stderrMode = io::Stdio::Pipe;
    version.killOnClose = true;
    auto reported = io::runToCompletion(io::realProcessLauncher(), version, "", 20s);
    ASSERT_TRUE(reported) << reported.error().describe();
    EXPECT_EQ(reported->exitCode, 0);
    EXPECT_NE((reported->stdoutBytes + reported->stderrBytes).find("Lua 5.4.9"), std::string::npos);
    frost::test::ScratchDir scratch("standalone-lua-run");
    auto manager = run::RunManager::create(io::realProcessLauncher());
    auto config = scriptConfiguration(scratch.path());
    frost::test::RunCollector script;
    ASSERT_TRUE(manager->start(config, script.handlers()));
    const auto finished = script.waitFinished();
    ASSERT_TRUE(finished.result);
    EXPECT_FALSE(finished.result->error);
    EXPECT_EQ(finished.result->exitCode, 0);
    EXPECT_NE(finished.out.find("STANDALONE_RESULT"), std::string::npos);
    EXPECT_NE(finished.out.find("43"), std::string::npos);
    EXPECT_NE(finished.out.find("Lua 5.4"), std::string::npos);
    config.id = "lua.repl";
    config.type = run::RunType::LuaRepl;
    config.target.clear();
    config.arguments.clear();
    frost::test::RunCollector repl;
    auto started = manager->start(config, repl.handlers());
    ASSERT_TRUE(started);
    auto running = repl.waitUntil([](const auto& state) {
        return std::find(state.phases.begin(), state.phases.end(), run::RunPhase::Running) != state.phases.end();
    });
    ASSERT_FALSE(running.result);
    ASSERT_TRUE(manager->sendInput(*started, "print('REAL_REPL_VALUE', 6*7)\nos.exit(0)\n"));
    const auto evaluated = repl.waitFinished();
    ASSERT_TRUE(evaluated.result);
    EXPECT_EQ(evaluated.result->exitCode, 0);
    EXPECT_NE(evaluated.out.find("REAL_REPL_VALUE"), std::string::npos);
    EXPECT_NE(evaluated.out.find("42"), std::string::npos);
}
TEST(StandaloneLuaReal, UnicodeScriptAndRequiredModuleUseSelectedInterpreter) {
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_INTERPRETER));
    ASSERT_TRUE(fs.status(FROST_TEST_LUA_INTERPRETER));
    frost::test::ScratchDir scratch("lua-unicode");
    const auto directory = scratch.join("räksmörgås-日本語");
    ASSERT_TRUE(fs.createDirectories(directory));
    const auto module = directory + "/modul_å.lua";
    const auto script = directory + "/test_日本語.lua";
    ASSERT_TRUE(fs.writeFileAtomic(module, "return 42\n"));
    ASSERT_TRUE(fs.writeFileAtomic(script, "assert(require('modul_å') == 42); print('UNICODE_MODULE_OK', arg[1])\n"));
    auto config = scriptConfiguration(directory);
    config.target = script;
    config.arguments = {"värde-日本語"};
    auto manager = run::RunManager::create(io::realProcessLauncher());
    frost::test::RunCollector collector;
    ASSERT_TRUE(manager->start(config, collector.handlers()));
    auto result = collector.waitFinished();
    ASSERT_TRUE(result.result);
    EXPECT_FALSE(result.result->error);
    EXPECT_EQ(result.result->exitCode, 0);
    EXPECT_NE(result.out.find("UNICODE_MODULE_OK"), std::string::npos);
    EXPECT_NE(result.out.find("värde-日本語"), std::string::npos);
}
TEST(StandaloneLuaReal, SameInterpreterBreakpointsLocalsUpvaluesTablesMutationStepsAndNewCoroutine) {
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_INTERPRETER));
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_DEBUG_ADAPTER))
        << "configure the opt-in Lua Debug coroutine provider";
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_COROUTINE_MODULE))
        << "configure the complete Lua runtime build, including the headers and import library";
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.status(FROST_TEST_LUA_INTERPRETER));
    ASSERT_TRUE(fs.status(FROST_TEST_LUA_DEBUG_ADAPTER));
    auto source = fs.readFile(FROST_TEST_LUA_SOURCE);
    ASSERT_TRUE(source);
    frost::test::ScratchDir scratch("standalone-lua-dap");
    const auto config = scriptConfiguration(scratch.path());
    const auto runPlan = run::launchPlanFor(config);
    ASSERT_TRUE(runPlan);
    StandaloneLuaTarget target;
    target.interpreter = runPlan->program;
    target.script = config.target;
    target.workingDirectory = runPlan->workingDirectory;
    target.arguments = config.arguments;
    target.interpreterArguments = config.interpreterArguments;
    target.environment.emplace_back("FROST_LUA_COROUTINE_TEST_MODULE", FROST_TEST_LUA_COROUTINE_MODULE);
    target.stopOnEntry = false;
    auto launch = standaloneLuaLaunchArguments(target);
    ASSERT_TRUE(launch);
    EXPECT_EQ(launch->stringAt("luaexe"), runPlan->program);
    const auto localLine = marker(*source, "STANDALONE_LOCALS");
    const auto coroutineLine = marker(*source, "STANDALONE_COROUTINE");
    const auto suspendedLine = marker(*source, "STANDALONE_SUSPENDED");
    const auto completedLine = marker(*source, "STANDALONE_AFTER_COMPLETION");
    const auto collectedLine = marker(*source, "STANDALONE_AFTER_COLLECTION");
    test::SessionRecorder recorder;
    auto owned = DebugSession::start(
        standaloneLuaAdapterFactory(io::realProcessLauncher(), FROST_TEST_LUA_DEBUG_ADAPTER, target),
        std::move(*launch),
        {{config.target,
          {{.id = 1, .line = localLine},
           {.id = 2, .line = coroutineLine},
           {.id = 3, .line = completedLine},
           {.id = 4, .line = collectedLine}}}},
        recorder.callbacks(), false, "lua");
    ASSERT_TRUE(owned);
    auto& session = *owned;
    ASSERT_TRUE(test::waitUntil([&] { return session.selectedFrame().has_value(); }));
    auto frame = session.selectedFrame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->line, localLine);
    EXPECT_FALSE(session.threads().empty());
    const auto mainThread = session.selectedThread();
    ASSERT_TRUE(mainThread);
    auto scopes = answer<ScopeList>([&](auto done) { session.scopes(frame->id, std::move(done)); });
    ASSERT_TRUE(scopes);
    bool captured = false, table = false;
    std::int64_t localReference = 0, inventoryReference = 0;
    for (const auto& scope : *scopes) {
        auto variables = answer<VariableList>(
            [&](auto done) { session.variables(scope.variablesReference, 0, 0, std::move(done)); });
        ASSERT_TRUE(variables);
        for (const auto& value : *variables) {
            if (value.name == "captured") {
                EXPECT_EQ(value.value, "17");
                captured = true;
            }
            if (value.name == "speed") {
                EXPECT_EQ(value.value, "25");
                localReference = scope.variablesReference;
            }
            if (value.name == "inventory") {
                EXPECT_GT(value.variablesReference, 0);
                inventoryReference = value.variablesReference;
                table = true;
            }
        }
    }
    EXPECT_TRUE(captured);
    EXPECT_TRUE(table);
    ASSERT_GT(localReference, 0);
    auto inventory =
        answer<VariableList>([&](auto done) { session.variables(inventoryReference, 0, 0, std::move(done)); });
    ASSERT_TRUE(inventory);
    const auto nested =
        std::find_if(inventory->begin(), inventory->end(), [](const auto& v) { return v.name == "nested"; });
    ASSERT_NE(nested, inventory->end());
    ASSERT_GT(nested->variablesReference, 0);
    auto members =
        answer<VariableList>([&](auto done) { session.variables(nested->variablesReference, 0, 0, std::move(done)); });
    ASSERT_TRUE(members);
    const auto fortyTwo =
        std::find_if(members->begin(), members->end(), [](const auto& v) { return v.name == "answer"; });
    ASSERT_NE(fortyTwo, members->end());
    EXPECT_EQ(fortyTwo->value, "42");
    auto repl = answer<dap::Variable>(
        [&](auto done) { session.evaluate("__frost_repl_probe = 123", frame->id, "repl", std::move(done)); });
    ASSERT_TRUE(repl);
    auto replEffect = answer<dap::Variable>(
        [&](auto done) { session.evaluate("__frost_repl_probe", frame->id, "watch", std::move(done)); });
    ASSERT_TRUE(replEffect);
    EXPECT_EQ(replEffect->value, "123");
    auto changed =
        answer<dap::Variable>([&](auto done) { session.setVariable(localReference, "speed", "26", std::move(done)); });
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->value, "26");
    auto watch = answer<dap::Variable>(
        [&](auto done) { session.evaluate("speed + captured", frame->id, "watch", std::move(done)); });
    ASSERT_TRUE(watch);
    EXPECT_EQ(watch->value, "43");
    auto restored =
        answer<dap::Variable>([&](auto done) { session.setVariable(localReference, "speed", "25", std::move(done)); });
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->value, "25");
    const auto stops = recorder.stopCount();
    ASSERT_TRUE(session.stepIn());
    ASSERT_TRUE(test::waitUntil([&] { return recorder.stopCount() > stops && session.selectedFrame().has_value(); }));
    EXPECT_EQ(session.selectedFrame()->line, marker(*source, "STANDALONE_STEP_IN"));
    const auto inside = recorder.stopCount();
    ASSERT_TRUE(session.stepOut());
    ASSERT_TRUE(test::waitUntil([&] { return recorder.stopCount() > inside && session.selectedFrame().has_value(); }));
    const auto steppedOut = recorder.stopCount();
    ASSERT_TRUE(session.next());
    ASSERT_TRUE(
        test::waitUntil([&] { return recorder.stopCount() > steppedOut && session.selectedFrame().has_value(); }));
    EXPECT_EQ(session.selectedFrame()->line, marker(*source, "STANDALONE_LEAF_RETURN"));
    const auto continued = recorder.stopCount();
    ASSERT_TRUE(session.continueRun());
    ASSERT_TRUE(
        test::waitUntil([&] { return recorder.stopCount() > continued && session.selectedFrame().has_value(); }));
    const auto coroutineFrame = session.selectedFrame();
    ASSERT_TRUE(coroutineFrame);
    EXPECT_EQ(coroutineFrame->line, coroutineLine);
    auto childValue = answer<dap::Variable>(
        [&](auto done) { session.evaluate("childValue", coroutineFrame->id, "watch", std::move(done)); });
    ASSERT_TRUE(childValue);
    EXPECT_EQ(childValue->value, "23");
    const auto childThread = session.selectedThread();
    ASSERT_TRUE(childThread);
    EXPECT_NE(*childThread, *mainThread) << "The C-created coroutine must have its own exposed thread identity";
    ASSERT_TRUE(test::waitUntil([&] { return session.threads().size() == 3; }));
    const auto threads = session.threads();
    const auto side = std::ranges::find_if(
        threads, [&](const DebugThread& thread) { return thread.id != *mainThread && thread.id != *childThread; });
    ASSERT_NE(side, threads.end());
    ASSERT_TRUE(session.selectThread(side->id));
    ASSERT_TRUE(test::waitUntil([&] { return !session.frames(side->id).frames.empty(); }));
    const auto sideFrames = session.frames(side->id).frames;
    const auto sideFrame = std::ranges::find(sideFrames, suspendedLine, &DebugFrame::line);
    ASSERT_NE(sideFrame, sideFrames.end());
    ASSERT_TRUE(session.selectFrame(side->id, sideFrame->id));
    auto sideValue = answer<dap::Variable>(
        [&](auto done) { session.evaluate("sideValue", sideFrame->id, "watch", std::move(done)); });
    ASSERT_TRUE(sideValue);
    EXPECT_EQ(sideValue->value, "73");
    auto childAgain = answer<dap::Variable>(
        [&](auto done) { session.evaluate("childValue", coroutineFrame->id, "watch", std::move(done)); });
    ASSERT_TRUE(childAgain);
    EXPECT_EQ(childAgain->value, "23");
    ASSERT_TRUE(session.continueRun());
    ASSERT_TRUE(test::waitUntil([&] {
        const auto selected = session.selectedFrame();
        return selected && selected->line == completedLine && session.selectedThread() == mainThread &&
               session.threads().size() == 2;
    }));
    EXPECT_FALSE(std::ranges::contains(session.threads(), *childThread, &DebugThread::id));
    EXPECT_TRUE(std::ranges::contains(session.threads(), side->id, &DebugThread::id));
    ASSERT_TRUE(session.continueRun());
    ASSERT_TRUE(test::waitUntil([&] {
        const auto selected = session.selectedFrame();
        return selected && selected->line == collectedLine && session.threads().size() == 1;
    }));
    EXPECT_EQ(session.threads().front().id, *mainThread);
    ASSERT_TRUE(session.continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    ASSERT_TRUE(session.outcome());
    EXPECT_FALSE(session.outcome()->error);
    EXPECT_EQ(session.outcome()->exitCode, 0);
    {
        LockGuard lock(recorder.mutex);
        EXPECT_NE(recorder.output.find("STANDALONE_RESULT"), std::string::npos);
    }
}
TEST(StandaloneLuaReal, OwnedTargetExitAndOutputSurviveAdapterEof) {
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_DEBUG_ADAPTER));
    frost::test::ScratchDir scratch("standalone-lua-exit");
    StandaloneLuaTarget target;
    target.interpreter = FROST_TEST_LUA_INTERPRETER;
    target.script = scratch.join("exit.lua");
    target.workingDirectory = scratch.path();
    target.interpreterArguments = {"-W"};
    target.arguments = {"two words"};
    target.environment = {{"FROST_LUA_DEBUG_FIXTURE", "admitted value"}};
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(
        target.script, "local marker = assert(io.open('observed', 'w')); marker:close()\n"
                       "print(arg[1], os.getenv('FROST_LUA_DEBUG_FIXTURE'))\nos.exit(7)\n"));
    auto launch = standaloneLuaLaunchArguments(target);
    ASSERT_TRUE(launch);
    test::SessionRecorder recorder;
    auto session = DebugSession::start(
        standaloneLuaAdapterFactory(io::realProcessLauncher(), FROST_TEST_LUA_DEBUG_ADAPTER, target),
        std::move(*launch), {}, recorder.callbacks(), false, "lua");
    ASSERT_TRUE(session);
    ASSERT_TRUE(test::waitUntil([&] { return session->selectedFrame().has_value() || recorder.hasEnded(); }));
    ASSERT_TRUE(session->selectedFrame());
    EXPECT_EQ(session->selectedFrame()->line, 1);
    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->error);
    EXPECT_EQ(outcome->exitCode, 7);
    EXPECT_TRUE(io::realFileSystem().status(scratch.join("observed")));
    LockGuard lock(recorder.mutex);
    EXPECT_NE(recorder.output.find("two words\tadmitted value"), std::string::npos);
}
} // namespace
} // namespace frost::debug
