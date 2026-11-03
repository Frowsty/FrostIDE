// SPDX-License-Identifier: Apache-2.0
//
// Tests for DebugSession (frost/debug/debug_session.hpp) against the fake-dap double's scripted
// program: the launch sequence, a breakpoint stop with threads and lazy frames, stepping, pause,
// an exception stop that selects the faulting frame, the exit, scopes, paged variables, evaluate,
// breakpoints changed during a session, Run to Cursor, Stop, an adapter crash and a failed start.

#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace frost::debug {
namespace {

using test::fakeFactory;
using test::SessionRecorder;
using test::waitUntil;
using test::workPath;

const std::string kSource = workPath("App/main.cpp");

TEST(DebugSessionAdapterProfile, InitializeCarriesTheSelectedAdapterId) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{});
    auto session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, {}, false, "coreclr");
    ASSERT_TRUE(adapter.waitForRequest("initialize"));
    ASSERT_EQ(adapter.argumentsOf("initialize").size(), 1U);
    EXPECT_EQ(adapter.argumentsOf("initialize").front().stringAt("adapterID"), "coreclr");
}

TEST(DebugSessionAdapterProfile, FrameIdZeroRemainsSelectableAndCanRequestScopes) {
    FakeDebugAdapter adapter;
    FakeProgram program{.sourcePath = kSource, .codeLines = {10}};
    scriptProgram(adapter, std::move(program));
    adapter.on("stackTrace", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{
            .body = rpc::Json::object()
                        .set("stackFrames",
                             rpc::Json::array({rpc::Json::object()
                                                   .set("id", 0)
                                                   .set("name", "Main")
                                                   .set("line", 10)
                                                   .set("column", 1)
                                                   .set("source", rpc::Json::object().set("path", kSource))}))
                        .set("totalFrames", 1)};
    });
    auto session =
        DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {{kSource, {{1, 10}}}}, {}, false, "coreclr");
    ASSERT_TRUE(waitUntil([&] { return session->selectedFrame().has_value(); }));
    EXPECT_EQ(session->selectedFrame()->id, 0);
    ASSERT_TRUE(session->selectFrame(1, 0));
    session->scopes(0, [](Result<ScopeList>) {});
    ASSERT_TRUE(adapter.waitForRequest("scopes"));
    EXPECT_EQ(adapter.argumentsOf("scopes").front().integerAt("frameId"), 0);
}

rpc::Json launchArguments() {
    return rpc::Json::object()
        .set("name", "App")
        .set("type", "lldb-dap")
        .set("request", "launch")
        .set("program", workPath("App/App.exe"))
        .set("stopOnEntry", false);
}

class DebugSessionTest : public ::testing::Test {
protected:
    void start(FakeProgram program, std::map<std::string, std::vector<BreakpointLine>> breakpoints) {
        program.sourcePath = kSource;
        scriptProgram(adapter, std::move(program));
        session =
            DebugSession::start(fakeFactory(adapter), launchArguments(), std::move(breakpoints), recorder.callbacks());
    }

    // Waits for stop number `count` (1-based) and its selected frame.
    void waitForStop(std::size_t count) {
        ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() >= count && recorder.selectedCount() >= count; }))
            << "events: " << testing::PrintToString(recorder.eventList());
    }

    // Runs a request and waits for its completion.
    template<typename T, typename Send>
    Result<T> awaitResult(Send send) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto future = promise->get_future();
        send([promise](Result<T> result) { promise->set_value(std::move(result)); });
        if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
            return fail(ErrorCode::Timeout, "no answer");
        }
        return future.get();
    }

    void TearDown() override { session.reset(); }

    FakeDebugAdapter adapter;
    SessionRecorder recorder;
    std::unique_ptr<DebugSession> session;
};

TEST_F(DebugSessionTest, LaunchesConfiguresBreakpointsAndStopsOnOne) {
    start(FakeProgram{.codeLines = {10, 20}}, {{kSource, {{1, 10}, {2, 11}}}});
    waitForStop(1);
    // The DAP launch sequence: launch right after initialize, breakpoints after "initialized".
    const auto commands = adapter.commands();
    ASSERT_GE(commands.size(), 6U);
    EXPECT_EQ(std::vector<std::string>(commands.begin(), commands.begin() + 5),
              (std::vector<std::string>{"initialize", "launch", "setBreakpoints", "configurationDone", "threads"}));
    EXPECT_GE(adapter.argumentsOf("threads").size(), 2U);
    const rpc::Json launch = adapter.argumentsOf("launch").front();
    EXPECT_EQ(launch.stringAt("program"), workPath("App/App.exe"));
    EXPECT_EQ(launch.boolAt("stopOnEntry"), false);
    const rpc::Json set = adapter.argumentsOf("setBreakpoints").front();
    EXPECT_EQ(set.find("source")->stringAt("path"), kSource);
    ASSERT_EQ(set.find("breakpoints")->size(), 2U);

    EXPECT_EQ(session->state(), DebugState::Stopped);
    EXPECT_TRUE(session->capabilities().supportsConfigurationDoneRequest);
    const auto stop = session->stopInfo();
    ASSERT_TRUE(stop.has_value());
    EXPECT_EQ(stop->reason, "breakpoint");
    EXPECT_EQ(stop->summary(), "Paused on breakpoint");
    EXPECT_FALSE(stop->isCrash());
    {
        LockGuard lock(recorder.mutex);
        ASSERT_EQ(recorder.breakpoints.size(), 2U);
        EXPECT_EQ(recorder.breakpoints[0], (BreakpointResult{.id = 1, .verified = true, .message = {}, .line = 10}));
        EXPECT_EQ(recorder.breakpoints[1].id, 2U);
        EXPECT_FALSE(recorder.breakpoints[1].verified);
        EXPECT_EQ(recorder.breakpoints[1].message, "No code is generated for this line");
        EXPECT_EQ(recorder.output, "hello from the debuggee\n");
        EXPECT_EQ(recorder.threads, (std::vector<DebugThread>{{1, "main"}, {2, "worker"}}));
    }
    // The stopped thread's frames, the top one selected at the breakpoint's line.
    EXPECT_EQ(session->selectedThread(), 1);
    const FrameList frames = session->frames(1);
    ASSERT_EQ(frames.frames.size(), 3U);
    EXPECT_TRUE(frames.complete);
    EXPECT_EQ(frames.frames[0].name, "main()");
    EXPECT_EQ(frames.frames[0].path, kSource);
    EXPECT_EQ(frames.frames[0].line, 10);
    EXPECT_FALSE(frames.frames[2].hasSource()) << "the outermost frame has no source";
    const auto selected = session->selectedFrame();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->id, 1000);
    // Another thread's frames load when it is selected.
    ASSERT_TRUE(session->selectThread(2).has_value());
    ASSERT_TRUE(waitUntil([&] { return recorder.selectedCount() >= 2; }));
    EXPECT_EQ(session->selectedFrame()->name, "worker_loop()");
    EXPECT_EQ(session->selectThread(77).error().code(), ErrorCode::NotFound);
}

struct InlineAdapterState {
    DapHandlers handlers;
    IDebugAdapterClient::Completion initialize;
    std::vector<std::string> commands;
    bool closed = false;
};
class InlineAdapter final : public IDebugAdapterClient {
public:
    explicit InlineAdapter(std::shared_ptr<InlineAdapterState> state) : m_state(std::move(state)) {}
    std::int64_t request(std::string command, rpc::Json, Completion done) override {
        m_state->commands.push_back(command);
        if (command == "initialize")
            m_state->initialize = std::move(done);
        else
            done(rpc::Json::object());
        return static_cast<std::int64_t>(m_state->commands.size());
    }
    void cancel(std::int64_t) override {}
    void setSupportsCancel(bool) noexcept override {}
    void close() noexcept override { m_state->closed = true; }
    bool closed() const noexcept override { return m_state->closed; }
    std::size_t pendingRequests() const override { return 0; }

private:
    std::shared_ptr<InlineAdapterState> m_state;
};

TEST_F(DebugSessionTest, RunningCallbackCanStopReentrantlyWithOrWithoutConfigurationDone) {
    for (const bool supportsDone : {false, true}) {
        auto state = std::make_shared<InlineAdapterState>();
        DebugSessionCallbacks callbacks;
        int runningCalls = 0;
        callbacks.onStateChanged = [&](DebugState value) {
            if (value == DebugState::Running) {
                ++runningCalls;
                session->stop(); // Must not run under startup serialization.
            }
        };
        AdapterFactory factory = [state](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
            state->handlers = std::move(handlers);
            return std::make_unique<InlineAdapter>(state);
        };
        session = DebugSession::start(std::move(factory), launchArguments(), {}, std::move(callbacks));
        ASSERT_TRUE(state->initialize);
        state->initialize(rpc::Json::object().set("supportsConfigurationDoneRequest", supportsDone));
        state->handlers.onEvent("initialized", rpc::Json());
        EXPECT_EQ(runningCalls, 1);
        EXPECT_NE(std::ranges::find(state->commands, "disconnect"), state->commands.end());
        session.reset();
    }
}

TEST_F(DebugSessionTest, AbortFunctionBreakpointSelectsTheUserFrame) {
    using Reply = FakeDebugAdapter::Reply;
    scriptProgram(adapter, FakeProgram{.sourcePath = kSource, .runsForever = true});
    adapter.on("initialize", [](const rpc::Json&) {
        return Reply{.body = rpc::Json::object()
                                 .set("supportsConfigurationDoneRequest", true)
                                 .set("supportsFunctionBreakpoints", true)};
    });
    adapter.on("setFunctionBreakpoints", [](const rpc::Json& args) {
        EXPECT_EQ(args.find("breakpoints")->items().front().stringAt("name"), "abort");
        return Reply{.body = rpc::Json::object().set(
                         "breakpoints", rpc::Json::array({rpc::Json::object().set("id", 77).set("verified", true)}))};
    });
    adapter.on("configurationDone", [](const rpc::Json&) {
        auto stop = rpc::Json::object()
                        .set("reason", "breakpoint")
                        .set("threadId", 1)
                        .set("hitBreakpointIds", rpc::Json::array({rpc::Json(77)}));
        return Reply{.events = {{"stopped", std::move(stop)}}};
    });
    adapter.on("stackTrace", [](const rpc::Json&) {
        auto crt = rpc::Json::object()
                       .set("id", 1)
                       .set("name", "abort")
                       .set("line", 61)
                       .set("source",
                            rpc::Json::object().set("path", "system/abort.cpp").set("presentationHint", "deemphasize"));
        auto user = rpc::Json::object()
                        .set("id", 2)
                        .set("name", "main")
                        .set("line", 10)
                        .set("source", rpc::Json::object().set("path", kSource));
        return Reply{.body =
                         rpc::Json::object().set("stackFrames", rpc::Json::array({crt, user})).set("totalFrames", 2)};
    });
    session = DebugSession::start(fakeFactory(adapter), launchArguments(), {}, recorder.callbacks(), true);
    waitForStop(1);
    ASSERT_TRUE(session->stopInfo().has_value());
    EXPECT_TRUE(session->stopInfo()->isCrash());
    EXPECT_EQ(session->stopInfo()->reason, "abort");
    ASSERT_TRUE(session->selectedFrame().has_value());
    EXPECT_EQ(session->selectedFrame()->path, kSource);
    EXPECT_EQ(session->selectedFrame()->line, 10);
}

TEST_F(DebugSessionTest, RejectedStepRestoresStoppedStateAndCanContinue) {
    start(FakeProgram{.codeLines = {10}}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    adapter.on("next",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "step rejected"}; });
    ASSERT_TRUE(session->next().has_value());
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Stopped && recorder.selectedCount() >= 2; }));
    ASSERT_TRUE(session->selectedFrame());
    EXPECT_EQ(session->selectedFrame()->line, 10);
    {
        LockGuard lock(recorder.mutex);
        EXPECT_NE(recorder.output.find("step rejected"), std::string::npos);
    }
    ASSERT_TRUE(session->continueRun().has_value());
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
}

TEST_F(DebugSessionTest, LateRejectedStepDoesNotOverwriteANewerStop) {
    start(FakeProgram{.codeLines = {10}}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    adapter.on("next", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    ASSERT_TRUE(session->next().has_value());
    ASSERT_TRUE(adapter.waitForRequest("next"));
    const auto requests = adapter.requests();
    const auto it = std::ranges::find_if(requests, [](const rpc::Json& r) { return r.stringAt("command") == "next"; });
    ASSERT_NE(it, requests.end());
    const auto seq = it->integerAt("seq");
    ASSERT_TRUE(seq);
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
    waitForStop(2);
    adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                         .set("seq", 999)
                                         .set("type", "response")
                                         .set("request_seq", *seq)
                                         .set("command", "next")
                                         .set("success", false)
                                         .set("message", "late")
                                         .dump()));
    adapter.sendReverseRequest("barrier", rpc::Json());
    ASSERT_TRUE(waitUntil([&] { return !adapter.reverseResponses().empty(); }));
    ASSERT_TRUE(session->stopInfo());
    EXPECT_EQ(session->stopInfo()->reason, "pause");
    EXPECT_EQ(session->state(), DebugState::Stopped);
}

TEST_F(DebugSessionTest, StopBeforeInitializeReplyPreventsLaunchAndConfiguration) {
    adapter.on("initialize", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    adapter.on("disconnect", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    session = DebugSession::start(fakeFactory(adapter), launchArguments(), {}, recorder.callbacks());
    ASSERT_TRUE(adapter.waitForRequest("initialize"));
    auto seq = adapter.requests().front().integerAt("seq");
    ASSERT_TRUE(seq);
    session->stop();
    ASSERT_TRUE(adapter.waitForRequest("disconnect"));
    adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                         .set("seq", 999)
                                         .set("type", "response")
                                         .set("request_seq", *seq)
                                         .set("command", "initialize")
                                         .set("success", true)
                                         .set("body", rpc::Json::object().set("supportsConfigurationDoneRequest", true))
                                         .dump()));
    adapter.sendEvent("initialized", rpc::Json());
    adapter.sendReverseRequest("unsupported-after-stop", rpc::Json());
    ASSERT_TRUE(waitUntil([&] { return !adapter.reverseResponses().empty(); }));
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    EXPECT_TRUE(adapter.argumentsOf("configurationDone").empty());
}

TEST_F(DebugSessionTest, StepsPausesAndRunsToTheExit) {
    start(FakeProgram{.codeLines = {10}, .exitCode = 7, .runsForever = false}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    EXPECT_EQ(session->pause().error().code(), ErrorCode::Busy) << "pause needs a running program";

    ASSERT_TRUE(session->next().has_value());
    waitForStop(2);
    EXPECT_EQ(session->stopInfo()->reason, "step");
    EXPECT_EQ(session->stopInfo()->summary(), "Paused after a step");
    EXPECT_EQ(session->selectedFrame()->line, 11);
    ASSERT_TRUE(session->stepIn().has_value());
    waitForStop(3);
    EXPECT_EQ(session->selectedFrame()->line, 12);
    ASSERT_TRUE(session->stepOut().has_value());
    waitForStop(4);
    EXPECT_EQ(session->selectedFrame()->line, 13);
    EXPECT_EQ(adapter.argumentsOf("next").front().integerAt("threadId"), 1);

    ASSERT_TRUE(session->continueRun().has_value());
    EXPECT_EQ(session->next().error().code(), ErrorCode::Busy) << "stepping needs a paused program";
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    EXPECT_EQ(session->state(), DebugState::Ended);
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->exitCode, 7);
    EXPECT_FALSE(outcome->error.has_value());
    const auto events = recorder.eventList();
    EXPECT_NE(std::ranges::find(events, "exited 7"), events.end());
    EXPECT_EQ(events.back(), "ended");
    EXPECT_TRUE(adapter.waitForRequest("disconnect")) << "terminated is answered with disconnect";
}

TEST_F(DebugSessionTest, PauseStopsARunningProgram) {
    start(FakeProgram{.codeLines = {}, .runsForever = true}, {});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    ASSERT_TRUE(session->pause().has_value());
    waitForStop(1);
    EXPECT_EQ(adapter.argumentsOf("pause").front().integerAt("threadId"), 1) << "the threads are asked for first";
    EXPECT_EQ(session->stopInfo()->reason, "pause");
    EXPECT_EQ(session->stopInfo()->summary(), "Paused");
}

TEST_F(DebugSessionTest, AnExceptionStopSelectsTheFaultingFrame) {
    start(FakeProgram{.codeLines = {10},
                      .entryLine = 30,
                      .crashReason = "Exception 0xc0000005 encountered at address 0x0: Access violation"},
          {});
    waitForStop(1);
    const auto stop = session->stopInfo();
    ASSERT_TRUE(stop.has_value());
    EXPECT_TRUE(stop->isCrash());
    EXPECT_EQ(stop->summary(), "Exception: Exception 0xc0000005 encountered at address 0x0: Access violation");
    const auto selected = session->selectedFrame();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->name, "crash()") << "not the runtime's abort frame";
    EXPECT_EQ(selected->line, 30);
    EXPECT_EQ(session->frames(1).frames.front().module, "ucrtbase.dll");
}

TEST_F(DebugSessionTest, FramesLoadAPageAtATime) {
    start(FakeProgram{.codeLines = {10}, .frameCount = 45}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    FrameList frames = session->frames(1);
    EXPECT_EQ(frames.frames.size(), static_cast<std::size_t>(DebugSession::kFramePage));
    EXPECT_FALSE(frames.complete);
    EXPECT_EQ(frames.totalFrames, 45);
    session->loadMoreFrames(1);
    ASSERT_TRUE(waitUntil([&] { return session->frames(1).frames.size() == 40; }));
    session->loadMoreFrames(1);
    ASSERT_TRUE(waitUntil([&] { return session->frames(1).complete; }));
    EXPECT_EQ(session->frames(1).frames.size(), 45U);
    const auto pages = adapter.argumentsOf("stackTrace");
    ASSERT_EQ(pages.size(), 3U);
    EXPECT_EQ(pages[1].integerAt("startFrame"), 20);
    EXPECT_EQ(pages[1].integerAt("levels"), DebugSession::kFramePage);
}

TEST_F(DebugSessionTest, ScopesVariablesPagingAndEvaluate) {
    start(FakeProgram{.codeLines = {10}, .arrayLength = 250}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    const auto frame = session->selectedFrame();
    ASSERT_TRUE(frame.has_value());

    auto scopes = awaitResult<ScopeList>([&](auto done) { session->scopes(frame->id, done); });
    ASSERT_TRUE(scopes.has_value()) << scopes.error().describe();
    ASSERT_EQ(scopes->size(), 2U);
    EXPECT_EQ((*scopes)[0].name, "Locals");
    EXPECT_TRUE((*scopes)[1].expensive);

    auto locals =
        awaitResult<VariableList>([&](auto done) { session->variables((*scopes)[0].variablesReference, 0, 0, done); });
    ASSERT_TRUE(locals.has_value());
    ASSERT_EQ(locals->size(), 3U);
    EXPECT_EQ((*locals)[0].name, "x");
    EXPECT_EQ((*locals)[0].value, "42");
    EXPECT_EQ((*locals)[0].type, "int");
    EXPECT_EQ((*locals)[1].variablesReference, 101) << "a struct has children";
    EXPECT_EQ((*locals)[2].indexedVariables, 250);

    auto members = awaitResult<VariableList>([&](auto done) { session->variables(101, 0, 0, done); });
    ASSERT_TRUE(members.has_value());
    EXPECT_EQ(members->size(), 2U);
    auto page = awaitResult<VariableList>([&](auto done) { session->variables(102, 100, 100, done); });
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->size(), 100U);
    EXPECT_EQ(page->front().name, "[100]");
    EXPECT_EQ(page->front().value, "1000");
    const rpc::Json paged = adapter.argumentsOf("variables").back();
    EXPECT_EQ(paged.integerAt("start"), 100);
    EXPECT_EQ(paged.integerAt("count"), 100);

    auto sum = awaitResult<dap::Variable>([&](auto done) { session->evaluate("x + 1", frame->id, "watch", done); });
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum->name, "x + 1");
    EXPECT_EQ(sum->value, "43");
    auto point = awaitResult<dap::Variable>([&](auto done) { session->evaluate("p", frame->id, "repl", done); });
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point->variablesReference, 101);
    auto missing = awaitResult<dap::Variable>([&](auto done) { session->evaluate("nope", frame->id, "watch", done); });
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message(), "evaluate: use of undeclared identifier 'nope'");
    const auto evaluations = adapter.argumentsOf("evaluate");
    EXPECT_EQ(evaluations.front().stringAt("context"), "watch");
    EXPECT_EQ(evaluations.front().integerAt("frameId"), frame->id);
}

TEST_F(DebugSessionTest, BreakpointsChangeDuringTheSessionAndRunToLineUsesATemporaryOne) {
    start(FakeProgram{.codeLines = {10, 20, 25, 40}, .runsForever = true}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    session->setBreakpoints(kSource, {{1, 10}, {5, 40}});
    ASSERT_TRUE(adapter.waitForRequest("setBreakpoints", 2));
    ASSERT_TRUE(waitUntil([&] {
        LockGuard lock(recorder.mutex);
        return recorder.breakpoints.size() >= 3;
    }));
    {
        LockGuard lock(recorder.mutex);
        EXPECT_EQ(recorder.breakpoints.back().id, 5U);
        EXPECT_TRUE(recorder.breakpoints.back().verified);
    }

    // Run to Cursor at line 25: a temporary breakpoint there, dropped again at the stop.
    ASSERT_TRUE(session->runToLine(kSource, 25).has_value());
    waitForStop(2);
    EXPECT_EQ(session->selectedFrame()->line, 25);
    ASSERT_TRUE(adapter.waitForRequest("setBreakpoints", 4));
    const auto sets = adapter.argumentsOf("setBreakpoints");
    EXPECT_EQ(sets[2].find("breakpoints")->size(), 3U) << "10, 40 and the temporary 25";
    EXPECT_EQ(sets[3].find("breakpoints")->size(), 2U) << "the temporary breakpoint is gone after the stop";
    {
        LockGuard lock(recorder.mutex);
        for (const BreakpointResult& result : recorder.breakpoints) {
            EXPECT_NE(result.id, 0U) << "the temporary breakpoint is never reported";
        }
    }
}

TEST_F(DebugSessionTest, LoadedSourceResponseExpiresWhenExecutionResumes) {
    start(FakeProgram{.codeLines = {10}, .runsForever = true}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    adapter.on("source", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    auto completion = std::make_shared<std::promise<Result<std::string>>>();
    auto future = completion->get_future();
    ASSERT_NE(
        session->source(91, [completion](Result<std::string> result) { completion->set_value(std::move(result)); }), 0);
    ASSERT_TRUE(adapter.waitForRequest("source"));
    const auto requests = adapter.requests();
    const auto request =
        std::ranges::find_if(requests, [](const rpc::Json& value) { return value.stringAt("command") == "source"; });
    ASSERT_NE(request, requests.end());
    const auto seq = request->integerAt("seq");
    ASSERT_TRUE(seq);
    ASSERT_TRUE(session->continueRun());
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                         .set("seq", 999)
                                         .set("type", "response")
                                         .set("request_seq", *seq)
                                         .set("command", "source")
                                         .set("success", true)
                                         .set("body", rpc::Json::object().set("content", "old revision"))
                                         .dump()));
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
}

TEST_F(DebugSessionTest, AttachedSessionDisconnectsWithoutTerminatingTheHost) {
    scriptProgram(adapter, FakeProgram{.codeLines = {10}, .runsForever = true});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(adapter.waitForRequest("attach"));
    session->stop();
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto disconnects = adapter.argumentsOf("disconnect");
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects.front().boolAt("terminateDebuggee"), false);
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->disconnectConfirmed);
}

TEST_F(DebugSessionTest, FailedAttachedDisconnectReportsUnconfirmedCleanup) {
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    adapter.on("disconnect",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "detach refused"}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    session->stop();
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(outcome->error);
    EXPECT_TRUE(outcome->stopped);
    EXPECT_FALSE(outcome->disconnectConfirmed);
    EXPECT_NE(outcome->error->message().find("disconnect was not confirmed"), std::string::npos);
    const auto disconnects = adapter.argumentsOf("disconnect");
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects.front().boolAt("terminateDebuggee"), false);
}

TEST_F(DebugSessionTest, ClosedBeforeDisconnectAnswerReportsUnconfirmedCleanup) {
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    adapter.on("disconnect", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    session->stop();
    ASSERT_TRUE(adapter.waitForRequest("disconnect"));
    adapter.crash(3);
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(outcome->error);
    EXPECT_NE(outcome->error->message().find("disconnect was not confirmed"), std::string::npos);
}

TEST_F(DebugSessionTest, FailedAttachAndDisconnectPreserveBothErrors) {
    scriptProgram(adapter, FakeProgram{});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "attach refused"}; });
    adapter.on("disconnect",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "detach refused"}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(outcome->error);
    EXPECT_NE(outcome->error->message().find("attach refused"), std::string::npos);
    EXPECT_NE(outcome->error->message().find("disconnect was not confirmed"), std::string::npos);
}

TEST_F(DebugSessionTest, ForcedCloseDoesNotConfirmDisconnect) {
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    session->kill();
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->stopped);
    EXPECT_FALSE(session->outcome()->disconnectConfirmed);
}

TEST_F(DebugSessionTest, FailedAttachDoesNotTerminateTheHost) {
    scriptProgram(adapter, FakeProgram{});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "refused"}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, recorder.callbacks(), false,
                                  DebugTargetMode::Attach);
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto disconnects = adapter.argumentsOf("disconnect");
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects.front().boolAt("terminateDebuggee"), false);
    ASSERT_TRUE(session->outcome().has_value());
    EXPECT_TRUE(session->outcome()->error.has_value());
}

TEST_F(DebugSessionTest, StopDisconnectsAndTerminatesTheDebuggee) {
    start(FakeProgram{.codeLines = {10}}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    session->stop();
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const rpc::Json disconnect = adapter.argumentsOf("disconnect").front();
    EXPECT_EQ(disconnect.boolAt("terminateDebuggee"), true);
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->stopped);
    EXPECT_FALSE(outcome->error.has_value());
    EXPECT_TRUE(adapter.closedByClient());
}

TEST_F(DebugSessionTest, AnAdapterCrashEndsTheSessionWithAnError) {
    start(FakeProgram{.codeLines = {10}}, {{kSource, {{1, 10}}}});
    waitForStop(1);
    adapter.crash(3);
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->error.has_value());
    EXPECT_NE(outcome->error->message().find("ended unexpectedly"), std::string::npos) << outcome->error->message();
    EXPECT_NE(outcome->error->message().find("exited with code 3"), std::string::npos);
    EXPECT_EQ(session->state(), DebugState::Ended);
}

TEST_F(DebugSessionTest, AFailedLaunchOrFactoryEndsTheSession) {
    scriptProgram(adapter, FakeProgram{.sourcePath = kSource});
    adapter.on("launch", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{.success = false, .message = "error: the program does not exist"};
    });
    session = DebugSession::start(fakeFactory(adapter), launchArguments(), {}, recorder.callbacks());
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    ASSERT_TRUE(session->outcome()->error.has_value());
    EXPECT_NE(session->outcome()->error->message().find("could not be started"), std::string::npos)
        << session->outcome()->error->message();

    SessionRecorder other;
    auto failed = DebugSession::start(
        [](DapHandlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
            return fail(ErrorCode::NotFound, "lldb-dap is not installed");
        },
        launchArguments(), {}, other.callbacks());
    EXPECT_EQ(failed->state(), DebugState::Ended);
    ASSERT_TRUE(failed->outcome().has_value());
    EXPECT_EQ(failed->outcome()->error->code(), ErrorCode::NotFound);
    EXPECT_TRUE(other.hasEnded());
}

} // namespace
} // namespace frost::debug
