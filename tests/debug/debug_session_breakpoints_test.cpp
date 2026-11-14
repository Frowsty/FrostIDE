// SPDX-License-Identifier: Apache-2.0
//
// Tests for DebugSession's DBG-1 and DBG-2a additions (frost/debug/debug_session.hpp) against the
// fake-dap double: function breakpoints sharing one request with the session's own, exception
// filters before configurationDone, the MSVC throw translation and its import-thunk skip, the
// filter a stop names, pause stops and the thread a pause selects, the capabilities event, restart,
// the process id and the attach failure hint; DBG-2b's thrown types, read through readMemory from a
// fake image, and the type list. The real adapter's side is dap_real_test.cpp and msvc_debug_test.cpp.

#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
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
using Reply = FakeDebugAdapter::Reply;

const std::string& kSource() {
    static const std::string value = workPath("App/main.cpp");
    return value;
}

rpc::Json launchArguments() {
    return rpc::Json::object()
        .set("name", "App")
        .set("type", "lldb-dap")
        .set("request", "launch")
        .set("program", workPath("App/App.exe"));
}

// lldb-dap 23's initialize answer, in the parts these tests use.
rpc::Json lldbCapabilities() {
    return rpc::Json::object()
        .set("supportsConfigurationDoneRequest", true)
        .set("supportsFunctionBreakpoints", true)
        .set("exceptionBreakpointFilters",
             rpc::Json::array({rpc::Json::object().set("filter", "cpp_catch").set("label", "C++ Catch"),
                               rpc::Json::object().set("filter", "cpp_throw").set("label", "C++ Throw")}));
}

std::vector<std::string> stringsOf(const rpc::Json* list, std::string_view member = {}) {
    std::vector<std::string> out;
    if (list == nullptr) {
        return out;
    }
    for (const rpc::Json& item : list->items()) {
        if (member.empty()) {
            out.emplace_back(item.asString() != nullptr ? *item.asString() : std::string());
        } else {
            out.emplace_back(item.stringAt(member));
        }
    }
    return out;
}

class SessionBreakpointsTest : public ::testing::Test {
protected:
    // Scripts a program that keeps running after configurationDone until the test sends events.
    void script() {
        scriptProgram(adapter, FakeProgram{.sourcePath = kSource(), .codeLines = {10}, .runsForever = true});
        adapter.on("initialize", [](const rpc::Json&) { return Reply{.body = lldbCapabilities()}; });
        adapter.on("configurationDone", [](const rpc::Json&) { return Reply{}; });
        adapter.on("setFunctionBreakpoints", [](const rpc::Json& arguments) {
            rpc::Json answers = rpc::Json::array();
            std::int64_t id = 100;
            for (const rpc::Json& breakpoint : arguments.find("breakpoints")->items()) {
                static_cast<void>(breakpoint);
                answers.push(rpc::Json::object().set("id", id++).set("verified", true));
            }
            return Reply{.body = rpc::Json::object().set("breakpoints", std::move(answers))};
        });
    }

    void start(DebugSessionOptions options) {
        session =
            DebugSession::start(fakeFactory(adapter), launchArguments(), {}, recorder.callbacks(), std::move(options));
        ASSERT_TRUE(adapter.waitForRequest("configurationDone"));
    }

    bool hasResult(std::uint64_t id) const {
        LockGuard lock(recorder.mutex);
        return std::ranges::any_of(recorder.breakpoints, [&](const BreakpointResult& r) { return r.id == id; });
    }

    std::size_t indexOf(std::string_view command) const {
        const auto commands = adapter.commands();
        return static_cast<std::size_t>(std::ranges::find(commands, command) - commands.begin());
    }

    FakeDebugAdapter adapter;
    SessionRecorder recorder;
    std::unique_ptr<DebugSession> session;
};

TEST_F(SessionBreakpointsTest, FunctionBreakpointsShareOneRequestWithAbortAndThrow) {
    script();
    start(DebugSessionOptions{.breakOnAbort = true,
                              .msvcExceptions = true,
                              .functionBreakpoints = {{.id = 1, .name = "frost_target", .condition = "value > 1"}},
                              .exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(waitUntil([&] { return hasResult(1); }));
    const auto requests = adapter.argumentsOf("setFunctionBreakpoints");
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(stringsOf(requests.front().find("breakpoints"), "name"),
              (std::vector<std::string>{"frost_target", "abort", "_CxxThrowException"}));
    EXPECT_EQ(requests.front().find("breakpoints")->items().front().stringAt("condition"), "value > 1");
    EXPECT_LT(indexOf("setFunctionBreakpoints"), indexOf("configurationDone"));
    LockGuard lock(recorder.mutex);
    ASSERT_EQ(recorder.breakpoints.size(), 1U) << "the session's own breakpoints are not reported";
    EXPECT_TRUE(recorder.breakpoints.front().verified);
}

TEST_F(SessionBreakpointsTest, MsvcFiltersAreTranslatedNotSent) {
    script();
    start(DebugSessionOptions{.msvcExceptions = true, .exceptionFilters = {"cpp_throw", "cpp_catch"}});
    ASSERT_TRUE(adapter.waitForRequest("setExceptionBreakpoints"));
    EXPECT_TRUE(stringsOf(adapter.argumentsOf("setExceptionBreakpoints").front().find("filters")).empty())
        << "lldb's Itanium filters never fire for the MSVC ABI";
    EXPECT_LT(indexOf("setExceptionBreakpoints"), indexOf("configurationDone"));
    EXPECT_TRUE(msvcUnsupportedFilter("cpp_catch").has_value());
    EXPECT_FALSE(msvcUnsupportedFilter("cpp_throw").has_value());
}

TEST_F(SessionBreakpointsTest, ItaniumFiltersGoToTheAdapterBeforeConfigurationDone) {
    script();
    start(DebugSessionOptions{.exceptionFilters = {"cpp_throw", "cpp_catch"}});
    ASSERT_TRUE(adapter.waitForRequest("setExceptionBreakpoints"));
    EXPECT_EQ(stringsOf(adapter.argumentsOf("setExceptionBreakpoints").front().find("filters")),
              (std::vector<std::string>{"cpp_throw", "cpp_catch"}));
    EXPECT_LT(indexOf("setExceptionBreakpoints"), indexOf("configurationDone"));
    EXPECT_FALSE(adapter.waitForRequest("setFunctionBreakpoints", 1, std::chrono::milliseconds(0)))
        << "nothing to replace";
}

TEST_F(SessionBreakpointsTest, ChangingFiltersLiveUpdatesTheThrowBreakpoint) {
    script();
    start(DebugSessionOptions{.msvcExceptions = true});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    session->setExceptionFilters({"cpp_throw"});
    ASSERT_TRUE(adapter.waitForRequest("setFunctionBreakpoints"));
    EXPECT_EQ(stringsOf(adapter.argumentsOf("setFunctionBreakpoints").back().find("breakpoints"), "name"),
              (std::vector<std::string>{"_CxxThrowException"}));
    session->setExceptionFilters({});
    ASSERT_TRUE(adapter.waitForRequest("setFunctionBreakpoints", 2));
    EXPECT_TRUE(stringsOf(adapter.argumentsOf("setFunctionBreakpoints").back().find("breakpoints")).empty());
    EXPECT_EQ(session->exceptionFilters(), std::vector<std::string>{});
}

TEST_F(SessionBreakpointsTest, MsvcThrowStopSkipsTheImportThunkAndNamesTheFilter) {
    script();
    // Frame 0 is the runtime's function (no source), frame 1 the thrower.
    adapter.on("stackTrace", [](const rpc::Json&) {
        return Reply{.body =
                         rpc::Json::object()
                             .set("stackFrames",
                                  rpc::Json::array({rpc::Json::object()
                                                        .set("id", 1000)
                                                        .set("name", "_CxxThrowException")
                                                        .set("line", 0)
                                                        .set("instructionPointerReference", "0x1000"),
                                                    rpc::Json::object()
                                                        .set("id", 1001)
                                                        .set("name", "thrower")
                                                        .set("line", 10)
                                                        .set("instructionPointerReference", "0x2000")
                                                        .set("source", rpc::Json::object().set("path", kSource()))}))
                             .set("totalFrames", 2)};
    });
    auto disassembled = std::make_shared<std::atomic<int>>(0);
    adapter.on("disassemble", [disassembled](const rpc::Json&) {
        const bool thunk = disassembled->fetch_add(1) == 0;
        return Reply{.body = rpc::Json::object().set(
                         "instructions", rpc::Json::array({rpc::Json::object()
                                                               .set("address", "0x1000")
                                                               .set("instruction", thunk ? "jmp qword ptr [rip + 0x2]"
                                                                                         : "push rbp")}))};
    });
    start(DebugSessionOptions{.msvcExceptions = true, .exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(adapter.waitForRequest("setFunctionBreakpoints"));
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    const auto throwStop = rpc::Json::object()
                               .set("reason", "breakpoint")
                               .set("threadId", 1)
                               .set("hitBreakpointIds", rpc::Json::array({rpc::Json(100)}));
    adapter.sendEvent("stopped", throwStop);
    ASSERT_TRUE(adapter.waitForRequest("continue")) << "the thunk's stop is resumed";
    EXPECT_EQ(recorder.stopCount(), 0U);
    adapter.sendEvent("stopped", throwStop);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && recorder.selectedCount() == 1; }));
    const StopInfo stop = *session->stopInfo();
    EXPECT_EQ(stop.reason, "exception");
    EXPECT_EQ(stop.exceptionFilter, "cpp_throw");
    EXPECT_EQ(stop.text, "C++ Throw");
    EXPECT_FALSE(stop.isCrash());
    EXPECT_EQ(stop.summary(), "Paused on exception (C++ Throw)");
    EXPECT_EQ(session->selectedFrame()->name, "thrower") << "the throwing frame, not the runtime's";
}

// Step into a thunk (DebugSession header comment): `instruction` is what the step-in's stop shows.
class StepThunkTest : public SessionBreakpointsTest, public ::testing::WithParamInterface<const char*> {
protected:
    // A stop at a sourced frame, then stepIn, whose first stop lands on a frame without a source.
    void stepIntoTheThunk() {
        script();
        adapter.on("initialize", [](const rpc::Json&) {
            return Reply{.body = lldbCapabilities()
                                     .set("supportsSteppingGranularity", true)
                                     .set("supportsDisassembleRequest", true)};
        });
        adapter.on("stackTrace", [this](const rpc::Json&) {
            rpc::Json frame = rpc::Json::object().set("id", 1000).set("instructionPointerReference", "0x1000");
            if (onThunk->load()) {
                frame.set("name", "0x1000").set("line", 0);
            } else {
                frame.set("name", "callee").set("line", 10).set("source", rpc::Json::object().set("path", kSource()));
            }
            return Reply{
                .body =
                    rpc::Json::object().set("stackFrames", rpc::Json::array({std::move(frame)})).set("totalFrames", 1)};
        });
        adapter.on("disassemble", [](const rpc::Json&) {
            return Reply{
                .body = rpc::Json::object().set(
                    "instructions",
                    rpc::Json::array({rpc::Json::object().set("address", "0x1000").set("instruction", GetParam())}))};
        });
        adapter.on("stepIn", [](const rpc::Json&) { return Reply{}; }); // The test sends the stops.
        start(DebugSessionOptions{});
        ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
        adapter.sendEvent("stopped", rpc::Json::object().set("reason", "breakpoint").set("threadId", 1));
        ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && session->selectedFrame().has_value(); }));
        onThunk->store(true);
        ASSERT_TRUE(session->stepIn());
        ASSERT_TRUE(adapter.waitForRequest("stepIn"));
        adapter.sendEvent("stopped", rpc::Json::object().set("reason", "step").set("threadId", 1));
    }

    std::shared_ptr<std::atomic<bool>> onThunk = std::make_shared<std::atomic<bool>>(false);
};

TEST_P(StepThunkTest, AStepInThatStopsOnAJmpWithoutSourceStepsOneMoreInstruction) {
    stepIntoTheThunk();
    const bool thunk = std::string_view(GetParam()).starts_with("jmp");
    if (!thunk) {
        ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 2; })) << "any other stop is reported as it came";
        EXPECT_EQ(adapter.argumentsOf("stepIn").size(), 1U);
        return;
    }
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 2; }));
    EXPECT_EQ(adapter.argumentsOf("stepIn").back().stringAt("granularity"), "instruction");
    EXPECT_EQ(recorder.stopCount(), 1U) << "the thunk's stop is not reported";
    onThunk->store(false);
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "step").set("threadId", 1));
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 2 && session->selectedFrame().has_value(); }));
    EXPECT_EQ(session->selectedFrame()->name, "callee");
    EXPECT_EQ(adapter.argumentsOf("stepIn").size(), 2U);
}

INSTANTIATE_TEST_SUITE_P(Instructions, StepThunkTest, ::testing::Values("jmp 0x7ff6374c1230", "push rbp"));

TEST_F(SessionBreakpointsTest, AStepInThroughAThunkIntoAStdFunctionStepsOutAndOnToTheNextCall) {
    script();
    adapter.on("initialize", [](const rpc::Json&) {
        return Reply{
            .body =
                lldbCapabilities().set("supportsSteppingGranularity", true).set("supportsDisassembleRequest", true)};
    });
    // The top frame at each stop: the line the step begins on, a thunk, the std:: function it leads
    // to, the line again (mid-line, after the std:: call), the next call's thunk, the callee.
    auto stage = std::make_shared<std::atomic<int>>(0);
    adapter.on("stackTrace", [stage](const rpc::Json&) {
        rpc::Json frame =
            rpc::Json::object().set("id", 1000 + stage->load()).set("instructionPointerReference", "0x1000");
        const auto sourced = [&](const char* name, std::int64_t line) {
            frame.set("name", name).set("line", line).set("source", rpc::Json::object().set("path", kSource()));
        };
        switch (stage->load()) {
        case 0:
        case 3: sourced("main", 10); break;
        case 1:
        case 4: frame.set("name", "0x1000").set("line", 0); break;
        case 2: sourced("std::basic_string_view<wchar_t>::basic_string_view(wchar_t const *const)", 40); break;
        default: sourced("Utf8::FromWide(std::wstring_view)", 20); break;
        }
        return Reply{
            .body = rpc::Json::object().set("stackFrames", rpc::Json::array({std::move(frame)})).set("totalFrames", 1)};
    });
    adapter.on("disassemble", [](const rpc::Json&) {
        return Reply{
            .body = rpc::Json::object().set(
                "instructions",
                rpc::Json::array({rpc::Json::object().set("address", "0x1000").set("instruction", "jmp 0x2000")}))};
    });
    adapter.on("stepIn", [](const rpc::Json&) { return Reply{}; });
    adapter.on("stepOut", [](const rpc::Json&) { return Reply{}; });
    start(DebugSessionOptions{});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    const auto step = rpc::Json::object().set("reason", "step").set("threadId", 1);
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "breakpoint").set("threadId", 1));
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && session->selectedFrame().has_value(); }));

    ASSERT_TRUE(session->stepIn());
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 1; }));
    stage->store(1);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 2; }));
    EXPECT_EQ(adapter.argumentsOf("stepIn").back().stringAt("granularity"), "instruction");
    stage->store(2);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepOut").size() == 1; })) << "out of the std:: function";
    stage->store(3);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 3; }));
    EXPECT_EQ(adapter.argumentsOf("stepIn").back().stringAt("granularity"), "") << "lldb's own step-in again";
    stage->store(4);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 4; }));
    EXPECT_EQ(recorder.stopCount(), 1U) << "nothing between was reported";
    stage->store(5);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 2 && session->selectedFrame().has_value(); }));
    EXPECT_EQ(session->selectedFrame()->name, "Utf8::FromWide(std::wstring_view)");
}

TEST_F(SessionBreakpointsTest, AnItaniumFilterStopNamesItsFilter) {
    script();
    start(DebugSessionOptions{.exceptionFilters = {"cpp_catch"}});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    adapter.sendEvent("stopped", rpc::Json::object()
                                     .set("reason", "exception")
                                     .set("description", "C++ Catch")
                                     .set("text", "C++ Catch")
                                     .set("threadId", 1));
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1; }));
    EXPECT_EQ(session->stopInfo()->exceptionFilter, "cpp_catch");
    EXPECT_FALSE(session->stopInfo()->isCrash());
}

TEST_F(SessionBreakpointsTest, AnExceptionWithoutAFilterIsACrash) {
    script();
    start(DebugSessionOptions{});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    adapter.sendEvent("stopped", rpc::Json::object()
                                     .set("reason", "exception")
                                     .set("description", "Exception 0xc0000005 encountered at address 0x0")
                                     .set("threadId", 1));
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1; }));
    EXPECT_TRUE(session->stopInfo()->isCrash());
    EXPECT_TRUE(session->stopInfo()->exceptionFilter.empty());
}

class PauseStopTest : public SessionBreakpointsTest, public ::testing::WithParamInterface<const char*> {};

// lldb-dap's pause stop on Linux and on Windows.
INSTANTIATE_TEST_SUITE_P(LldbDap, PauseStopTest,
                         ::testing::Values("signal SIGSTOP", "Exception 0x80000003 encountered at address 0x1"));

TEST_P(PauseStopTest, IsReportedAsAPause) {
    script();
    const std::string description = GetParam();
    adapter.on("pause", [description](const rpc::Json&) {
        return Reply{
            .events = {
                {"stopped",
                 rpc::Json::object().set("reason", "exception").set("description", description).set("threadId", 1)}}};
    });
    start(DebugSessionOptions{});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1; })) << description;
    EXPECT_EQ(session->stopInfo()->reason, "pause") << description;
    EXPECT_FALSE(session->stopInfo()->isCrash());
}

TEST_F(SessionBreakpointsTest, APauseOnAThreadWithoutSourceSelectsOneWithSource) {
    script();
    adapter.on("threads", [](const rpc::Json&) {
        return Reply{.body = rpc::Json::object().set(
                         "threads", rpc::Json::array({rpc::Json::object().set("id", 1).set("name", "main"),
                                                      rpc::Json::object().set("id", 3).set("name", "break-in")}))};
    });
    adapter.on("stackTrace", [](const rpc::Json& arguments) {
        const std::int64_t thread = arguments.integerAt("threadId").value_or(0);
        rpc::Json frame = rpc::Json::object().set("id", thread * 1000).set("line", 7);
        if (thread == 3) {
            frame.set("name", "DbgBreakPoint")
                .set("source", rpc::Json::object()
                                   .set("path", "ntdll.dll`DbgBreakPoint")
                                   .set("sourceReference", 9)
                                   .set("presentationHint", "deemphasize"));
        } else {
            frame.set("name", "spin").set("source", rpc::Json::object().set("path", kSource()));
        }
        return Reply{
            .body = rpc::Json::object().set("stackFrames", rpc::Json::array({std::move(frame)})).set("totalFrames", 1)};
    });
    adapter.on("pause", [](const rpc::Json&) {
        return Reply{.events = {{"stopped", rpc::Json::object()
                                                .set("reason", "exception")
                                                .set("description", "Exception 0x80000003 encountered")
                                                .set("threadId", 3)}}};
    });
    start(DebugSessionOptions{});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    ASSERT_TRUE(session->pause());
    ASSERT_TRUE(waitUntil([&] { return recorder.selectedCount() >= 1; }));
    EXPECT_EQ(session->selectedThread(), 1);
    EXPECT_EQ(session->selectedFrame()->name, "spin");
}

TEST_F(SessionBreakpointsTest, RestartWaitsForTheCapabilityAndSendsTheLaunchArguments) {
    script();
    start(DebugSessionOptions{});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    const auto refused = session->restart();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Unsupported);
    adapter.sendEvent("capabilities",
                      rpc::Json::object().set("capabilities", rpc::Json::object().set("supportsRestartRequest", true)));
    ASSERT_TRUE(waitUntil([&] { return session->capabilities().supportsRestartRequest; }));
    EXPECT_TRUE(session->capabilities().supportsFunctionBreakpoints) << "merged, not replaced";
    ASSERT_TRUE(session->restart());
    ASSERT_TRUE(adapter.waitForRequest("restart"));
    const rpc::Json arguments = adapter.argumentsOf("restart").front();
    ASSERT_NE(arguments.find("arguments"), nullptr);
    EXPECT_EQ(arguments.find("arguments")->stringAt("program"), workPath("App/App.exe"));
}

TEST_F(SessionBreakpointsTest, TheProcessEventGivesTheProcessId) {
    script();
    start(DebugSessionOptions{});
    EXPECT_FALSE(session->processId().has_value());
    adapter.sendEvent("process",
                      rpc::Json::object().set("name", "App").set("systemProcessId", 4321).set("startMethod", "launch"));
    ASSERT_TRUE(waitUntil([&] { return session->processId().has_value(); }));
    EXPECT_EQ(*session->processId(), 4321);
}

TEST_F(SessionBreakpointsTest, AFailedAttachCarriesTheHint) {
    script();
    adapter.on("attach", [](const rpc::Json&) { return Reply{.success = false, .message = "no such process"}; });
    session = DebugSession::start(
        fakeFactory(adapter), rpc::Json::object().set("request", "attach").set("pid", 1), {}, recorder.callbacks(),
        DebugSessionOptions{.targetMode = DebugTargetMode::Attach, .attachHint = "Linux refused the attach: hint"});
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome && outcome->error);
    EXPECT_TRUE(outcome->error->message().starts_with("Could not attach to the program: "))
        << outcome->error->message();
    EXPECT_NE(outcome->error->message().find("no such process"), std::string::npos);
    EXPECT_NE(outcome->error->message().find("Linux refused the attach: hint"), std::string::npos);
}

TEST_F(SessionBreakpointsTest, AnAttachThatFailsAtConfigurationDoneSaysItIsAnAttach) {
    script();
    adapter.on("attach", [](const rpc::Json&) { return Reply{.events = {{"initialized", rpc::Json()}}}; });
    adapter.on("configurationDone",
               [](const rpc::Json&) { return Reply{.success = false, .message = "Expected process to be stopped."}; });
    session = DebugSession::start(fakeFactory(adapter), rpc::Json::object().set("request", "attach").set("pid", 1), {},
                                  recorder.callbacks(),
                                  DebugSessionOptions{.targetMode = DebugTargetMode::Attach, .attachHint = "hint"});
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome && outcome->error);
    EXPECT_TRUE(outcome->error->message().starts_with("Could not attach to the program: "))
        << outcome->error->message();
    EXPECT_NE(outcome->error->message().find("Expected process to be stopped."), std::string::npos);
    EXPECT_TRUE(outcome->error->message().ends_with("\nhint"));
}

TEST_F(SessionBreakpointsTest, WithStepOutOfNoDebugAStepInLeavesAFunctionWithoutDebugInformation) {
    script();
    adapter.on("initialize", [](const rpc::Json&) {
        return Reply{
            .body =
                lldbCapabilities().set("supportsSteppingGranularity", true).set("supportsDisassembleRequest", true)};
    });
    // The top frame at each stop: the line the step begins on, a function without debug information
    // (not a jmp), the line again (after the call), the callee.
    auto stage = std::make_shared<std::atomic<int>>(0);
    adapter.on("stackTrace", [stage](const rpc::Json&) {
        rpc::Json frame =
            rpc::Json::object().set("id", 1000 + stage->load()).set("instructionPointerReference", "0x1000");
        const auto sourced = [&](const char* name, std::int64_t line) {
            frame.set("name", name).set("line", line).set("source", rpc::Json::object().set("path", kSource()));
        };
        switch (stage->load()) {
        case 0:
        case 2: sourced("main", 10); break;
        case 1: // lldb-dap's disassembly listing of a function without debug information.
            frame.set("name", "KERNELBASE.dll`Sleep")
                .set("line", 130)
                .set("source", rpc::Json::object()
                                   .set("path", "KERNELBASE.dll`Sleep")
                                   .set("sourceReference", 9)
                                   .set("presentationHint", "deemphasize"));
            break;
        default: sourced("callee(int)", 20); break;
        }
        return Reply{
            .body = rpc::Json::object().set("stackFrames", rpc::Json::array({std::move(frame)})).set("totalFrames", 1)};
    });
    adapter.on("disassemble", [](const rpc::Json&) {
        return Reply{
            .body = rpc::Json::object().set(
                "instructions",
                rpc::Json::array({rpc::Json::object().set("address", "0x1000").set("instruction", "push rbp")}))};
    });
    adapter.on("stepIn", [](const rpc::Json&) { return Reply{}; });
    adapter.on("stepOut", [](const rpc::Json&) { return Reply{}; });
    start(DebugSessionOptions{.stepOutOfNoDebug = true});
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    const auto step = rpc::Json::object().set("reason", "step").set("threadId", 1);
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "breakpoint").set("threadId", 1));
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && session->selectedFrame().has_value(); }));

    ASSERT_TRUE(session->stepIn());
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 1; }));
    stage->store(1);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepOut").size() == 1; })) << "out of Sleep";
    stage->store(2);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return adapter.argumentsOf("stepIn").size() == 2; })) << "on to the line's next call";
    EXPECT_EQ(recorder.stopCount(), 1U) << "nothing between was reported";
    stage->store(3);
    adapter.sendEvent("stopped", step);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 2 && session->selectedFrame().has_value(); }));
    EXPECT_EQ(session->selectedFrame()->name, "callee(int)");
}

// --- DBG-2b: thrown types ---

// Base64 of `bytes` (DAP readMemory's data).
std::string base64(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            chunk |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size())
            chunk |= static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 63]);
        out.push_back(kAlphabet[(chunk >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[chunk & 63] : '=');
    }
    return out;
}

/*
 * A fake x64 image at kImageBase holding two throws' structures (msvc_throw.hpp): ThrowInfo kRange
 * of std::range_error (two catchable types: itself and its base std::runtime_error) and kRuntime
 * of std::runtime_error.
 */
class FakeImage {
public:
    static constexpr std::uint64_t kImageBase = 0x140000000;
    static constexpr std::uint64_t kRange = kImageBase + 0x1000;
    static constexpr std::uint64_t kRuntime = kImageBase + 0x1100;

    FakeImage() : m_bytes(0x8000, '\0') {
        m_bytes[0] = 'M';
        m_bytes[1] = 'Z';
        put(0x3C, 0x80);
        m_bytes.replace(0x80, 4, std::string("PE\0\0", 4));
        m_bytes[0x84] = static_cast<char>(0x64);
        m_bytes[0x85] = static_cast<char>(0x86);
        put(0x80 + 4 + 20 + 56, static_cast<std::uint32_t>(m_bytes.size()));
        // ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor (+16: the name).
        put(0x1000 + 12, 0x2000);
        put(0x2000, 2);
        put(0x2004, 0x3000);
        put(0x2008, 0x3100);
        put(0x3000 + 4, 0x4000);
        name(0x4000, ".?AVrange_error@std@@");
        put(0x1100 + 12, 0x2100);
        put(0x2100, 1);
        put(0x2104, 0x3100);
        put(0x3100 + 4, 0x4100);
        name(0x4100, ".?AVruntime_error@std@@");
    }

    // readMemory's answer for `arguments`.
    [[nodiscard]] Reply read(const rpc::Json& arguments) const {
        const std::string reference(arguments.stringAt("memoryReference"));
        const std::uint64_t address = std::stoull(reference, nullptr, 16);
        const auto count = static_cast<std::size_t>(arguments.integerAt("count").value_or(0));
        if (address < kImageBase || address - kImageBase >= m_bytes.size()) {
            return Reply{.success = false, .message = "memory read failed"};
        }
        const std::string bytes = m_bytes.substr(address - kImageBase, count);
        return Reply{.body = rpc::Json::object().set("address", reference).set("data", base64(bytes))};
    }

private:
    void put(std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i)
            m_bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    void name(std::size_t descriptor, std::string_view decorated) {
        m_bytes.replace(descriptor + 16, decorated.size(), decorated);
    }

    std::string m_bytes;
};

class ThrowTypesTest : public SessionBreakpointsTest {
protected:
    // A session whose throws stop in the DLL runtime's _CxxThrowException (no source, not a jmp)
    // with rdx = the ThrowInfo next() picks.
    void startThrowing(std::vector<std::string> types) {
        script();
        adapter.on("stackTrace", [](const rpc::Json&) {
            return Reply{
                .body = rpc::Json::object()
                            .set("stackFrames",
                                 rpc::Json::array({rpc::Json::object()
                                                       .set("id", 1000)
                                                       .set("name", "_CxxThrowException")
                                                       .set("line", 0)
                                                       .set("instructionPointerReference", "0x7ff800001000"),
                                                   rpc::Json::object()
                                                       .set("id", 1001)
                                                       .set("name", "thrower")
                                                       .set("line", 10)
                                                       .set("instructionPointerReference", "0x140001234")
                                                       .set("source", rpc::Json::object().set("path", kSource()))}))
                            .set("totalFrames", 2)};
        });
        adapter.on("disassemble", [](const rpc::Json&) {
            return Reply{.body = rpc::Json::object().set(
                             "instructions", rpc::Json::array({rpc::Json::object()
                                                                   .set("address", "0x7ff800001000")
                                                                   .set("instruction", "movq %rdx, 0x10(%rsp)")}))};
        });
        adapter.on("evaluate", [this](const rpc::Json& arguments) {
            evaluated->emplace_back(arguments.stringAt("expression"));
            return Reply{.body = rpc::Json::object().set("result", std::format("0x{:016X}", next->load()))};
        });
        adapter.on("modules", [](const rpc::Json&) {
            return Reply{
                .body = rpc::Json::object().set(
                    "modules",
                    rpc::Json::array(
                        {rpc::Json::object().set("name", "App.exe").set("addressRange", "0x140000000"),
                         rpc::Json::object().set("name", "vcruntime140d.dll").set("addressRange", "0x7ff800000000")}))};
        });
        adapter.on("readMemory", [this](const rpc::Json& arguments) { return image.read(arguments); });
        start(DebugSessionOptions{
            .msvcExceptions = true, .exceptionFilters = {"cpp_throw"}, .throwTypes = std::move(types)});
        ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
    }

    void throwAt(std::uint64_t throwInfo) {
        next->store(throwInfo);
        adapter.sendEvent("stopped", rpc::Json::object()
                                         .set("reason", "breakpoint")
                                         .set("threadId", 1)
                                         .set("hitBreakpointIds", rpc::Json::array({rpc::Json(100)})));
    }

    FakeImage image;
    std::shared_ptr<std::atomic<std::uint64_t>> next = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<std::vector<std::string>> evaluated = std::make_shared<std::vector<std::string>>();
};

TEST_F(ThrowTypesTest, ATypeListResumesOtherThrowsAndStopsForItsType) {
    startThrowing({"std::runtime_error"});
    throwAt(FakeImage::kRange);
    ASSERT_TRUE(adapter.waitForRequest("continue")) << "std::range_error is not in the list";
    EXPECT_EQ(recorder.stopCount(), 0U);
    EXPECT_EQ(adapter.argumentsOf("evaluate").front().stringAt("expression"), "$rdx")
        << "the runtime has no symbols: the argument register";
    throwAt(FakeImage::kRuntime);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && recorder.selectedCount() == 1; }));
    const StopInfo stop = *session->stopInfo();
    EXPECT_EQ(stop.exceptionFilter, "cpp_throw");
    EXPECT_EQ(stop.exceptionType, "std::runtime_error");
    EXPECT_EQ(stop.description, "C++ exception thrown: std::runtime_error");
    EXPECT_EQ(stop.summary(), "Paused on exception (C++ Throw)");
    EXPECT_EQ(session->selectedFrame()->name, "thrower");
    EXPECT_EQ(adapter.argumentsOf("modules").size(), 1U);

    // A second std::range_error: the cached type, one evaluate and no memory read.
    const std::size_t reads = adapter.argumentsOf("readMemory").size();
    ASSERT_TRUE(session->continueRun().has_value());
    ASSERT_TRUE(adapter.waitForRequest("continue", 2));
    throwAt(FakeImage::kRange);
    ASSERT_TRUE(adapter.waitForRequest("continue", 3));
    EXPECT_EQ(adapter.argumentsOf("readMemory").size(), reads);
    EXPECT_EQ(adapter.argumentsOf("modules").size(), 1U);
    EXPECT_EQ(recorder.stopCount(), 1U);
}

TEST_F(ThrowTypesTest, WithoutATypeListEveryThrowStopsNamingItsType) {
    startThrowing({});
    throwAt(FakeImage::kRange);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1 && recorder.selectedCount() == 1; }));
    EXPECT_EQ(session->stopInfo()->exceptionType, "std::range_error");
    EXPECT_TRUE(adapter.argumentsOf("continue").empty());
}

TEST_F(ThrowTypesTest, AThrowWhoseTypeCannotBeReadStopsAndSaysWhy) {
    startThrowing({"std::runtime_error"});
    // Above the runtime's base, whose memory the fake cannot read: the PE header check fails.
    throwAt(0x7ff900000000);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1; }));
    EXPECT_EQ(session->stopInfo()->exceptionType, "");
    EXPECT_EQ(session->stopInfo()->exceptionFilter, "cpp_throw");
    LockGuard lock(recorder.mutex);
    EXPECT_NE(recorder.output.find("The thrown type could not be read"), std::string::npos) << recorder.output;
}

TEST_F(ThrowTypesTest, TypeListsChangeDuringTheSession) {
    startThrowing({"std::logic_error"});
    EXPECT_EQ(session->throwTypes(), std::vector<std::string>{"std::logic_error"});
    session->setThrowTypes({"std::range_error"});
    throwAt(FakeImage::kRange);
    ASSERT_TRUE(waitUntil([&] { return recorder.stopCount() == 1; }));
    EXPECT_EQ(session->stopInfo()->exceptionType, "std::range_error");
}

TEST_F(SessionBreakpointsTest, AnItaniumSessionSaysItIgnoresThrowTypes) {
    script();
    start(DebugSessionOptions{.exceptionFilters = {"cpp_throw"}, .throwTypes = {"std::runtime_error"}});
    ASSERT_TRUE(waitUntil([&] {
        LockGuard lock(recorder.mutex);
        return recorder.output.find("Exception type filters work only") != std::string::npos;
    }));
    session->setThrowTypes({"other"});
    LockGuard lock(recorder.mutex);
    const std::string& output = recorder.output;
    EXPECT_EQ(output.find("Exception type filters work only"), output.rfind("Exception type filters work only"))
        << "said once";
}

} // namespace
} // namespace frost::debug
