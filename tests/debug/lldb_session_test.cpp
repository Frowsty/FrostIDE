// SPDX-License-Identifier: Apache-2.0
//
// The real-adapter test (label "lldb"): starts the pinned lldb-dap through the real process
// launcher, debugs tests/debug/fixtures/debuggee.cpp, stops on a breakpoint, reads a local and runs
// the program to its exit. It skips, saying why, while lldb-dap is not installed.

#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <future>
#include <map>
#include <string>
#include <vector>

#ifndef FROST_DEBUGGEE_PATH
#error "FROST_DEBUGGEE_PATH comes from tests/debug/CMakeLists.txt"
#endif

namespace frost::debug {
namespace {

using namespace std::chrono_literals;
using test::SessionRecorder;
using test::waitUntil;

// Runs a request and waits up to 60 s for its completion.
template<typename T, typename Send>
Result<T> awaitResult(Send send) {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    send([promise](Result<T> result) { promise->set_value(std::move(result)); });
    if (future.wait_for(60s) != std::future_status::ready) {
        return fail(ErrorCode::Timeout, "lldb-dap did not answer in 60 s");
    }
    return future.get();
}

// The 1-based line of the fixture source holding `marker`; 0 when it is missing.
std::int64_t lineOf(std::string_view source, std::string_view marker) {
    std::int64_t line = 1;
    std::size_t start = 0;
    while (start < source.size()) {
        const std::size_t end = std::min(source.find('\n', start), source.size());
        if (source.substr(start, end - start).find(marker) != std::string_view::npos) {
            return line;
        }
        start = end + 1;
        ++line;
    }
    return 0;
}

TEST(LldbDebugSession, BreaksInAFixtureProgramAndReadsALocal) {
    io::IFileSystem& fs = io::realFileSystem();
    auto adapterPath = findLldbDap(fs, defaultLldbDapSearch());
    // A missing pinned tool fails the test, never skips it (FND-6).
    ASSERT_TRUE(adapterPath.has_value()) << "the pinned lldb-dap is required: " << adapterPath.error().message();
    const std::string program = io::normalizePath(FROST_DEBUGGEE_PATH).value();
    const std::string sourcePath = io::normalizePath(FROST_DEBUGGEE_SOURCE).value();
    auto source = fs.readFile(sourcePath);
    ASSERT_TRUE(source.has_value()) << source.error().describe();
    const std::int64_t breakLine = lineOf(*source, "// FROST_BREAK_HERE");
    ASSERT_GT(breakLine, 0);

    DebugTarget target{.name = "debuggee", .program = program};
    SessionRecorder recorder;
    std::map<std::string, std::vector<BreakpointLine>> breakpoints{{sourcePath, {{1, breakLine}}}};
    auto session = DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), *adapterPath),
                                       lldbDapLaunchArguments(target), std::move(breakpoints), recorder.callbacks());
    // What the session saw, for a failure message.
    const auto report = [&] {
        LockGuard lock(recorder.mutex);
        std::string text = "events: " + testing::PrintToString(recorder.events) + "\nbreakpoints:";
        for (const BreakpointResult& result : recorder.breakpoints) {
            text += std::format(" [{} verified={} '{}']", result.id, result.verified, result.message);
        }
        text += "\noutput: " + recorder.output;
        if (recorder.ended && recorder.ended->error) {
            text += "\nerror: " + recorder.ended->error->describe();
        }
        return text;
    };
    ASSERT_TRUE(waitUntil(
        [&] { return (recorder.stopCount() >= 1 && recorder.selectedCount() >= 1) || recorder.hasEnded(); }, 60s))
        << report();
    ASSERT_FALSE(recorder.hasEnded()) << "the program ended without stopping\n" << report();
    {
        LockGuard lock(recorder.mutex);
        ASSERT_FALSE(recorder.breakpoints.empty());
        EXPECT_TRUE(recorder.breakpoints.front().verified) << recorder.breakpoints.front().message;
    }
    EXPECT_EQ(session->stopInfo()->reason, "breakpoint");
    const auto frame = session->selectedFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->line, breakLine);
    EXPECT_NE(frame->name.find("compute"), std::string::npos) << frame->name;

    auto scopes = awaitResult<ScopeList>([&](auto done) { session->scopes(frame->id, done); });
    ASSERT_TRUE(scopes.has_value()) << scopes.error().describe();
    const auto locals = std::ranges::find_if(
        *scopes, [](const dap::Scope& scope) { return scope.presentationHint == "locals" || scope.name == "Locals"; });
    ASSERT_NE(locals, scopes->end());
    auto variables =
        awaitResult<VariableList>([&](auto done) { session->variables(locals->variablesReference, 0, 0, done); });
    ASSERT_TRUE(variables.has_value()) << variables.error().describe();
    const auto local = std::ranges::find(*variables, std::string("local"), &dap::Variable::name);
    ASSERT_NE(local, variables->end());
    EXPECT_EQ(local->value, "42");

    auto sum = awaitResult<dap::Variable>([&](auto done) { session->evaluate("local + 1", frame->id, "watch", done); });
    ASSERT_TRUE(sum.has_value()) << sum.error().describe();
    EXPECT_EQ(sum->value, "43");

    ASSERT_TRUE(session->continueRun().has_value());
    ASSERT_TRUE(waitUntil([&] { return recorder.hasEnded(); }, 60s));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->error.has_value()) << (outcome->error ? outcome->error->describe() : "");
    EXPECT_EQ(outcome->exitCode, 0);
}

} // namespace
} // namespace frost::debug
