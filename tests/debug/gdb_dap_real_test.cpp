// SPDX-License-Identifier: Apache-2.0
// Authored for later Linux execution; missing tools fail rather than skip.
#include "frost/debug/gdb_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/project/vcxproj.hpp"

#include "debug_test_util.hpp"

#include <algorithm>
#include <future>

namespace frost::debug {
TEST(GdbDapReal, BreakpointThreadsStackValueStepAndNormalExit) {
    auto& fs = io::realFileSystem();
    auto gdb = findGdbDap(fs, project::processEnvironmentVariable("FROST_GDB_PATH").value_or(""));
    ASSERT_TRUE(gdb) << gdb.error().describe();
    const std::string source = FROST_DEBUGGEE_SOURCE;
    auto contents = fs.readFile(source);
    ASSERT_TRUE(contents);
    const auto marker = contents->find("// FROST_BREAK_HERE");
    ASSERT_NE(marker, std::string::npos);
    const auto line = 1 + std::count(contents->begin(), contents->begin() + marker, '\n');
    auto trust = project::TrustStore::open(fs, FROST_GDB_TRUST_PATH);
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(std::string(io::parentPath(FROST_DEBUGGEE_PATH))));
    DebugTarget target{.program = FROST_DEBUGGEE_PATH, .inheritEnvironment = false};
    test::SessionRecorder recorder;
    auto session = startGdbDebugSession(**trust, target, gdbDapAdapterFactory(io::realProcessLauncher(), *gdb), {},
                                        {{source, {{.id = 1, .line = line}}}}, recorder.callbacks());
    ASSERT_TRUE(session) << session.error().describe();
    ASSERT_TRUE(test::waitUntil([&] { return (*session)->selectedFrame().has_value(); }));
    auto frame = (*session)->selectedFrame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->line, line);
    EXPECT_FALSE((*session)->threads().empty());
    auto scopePromise = std::make_shared<std::promise<Result<ScopeList>>>();
    auto scopeFuture = scopePromise->get_future();
    (*session)->scopes(frame->id, [scopePromise](auto answer) { scopePromise->set_value(std::move(answer)); });
    ASSERT_EQ(scopeFuture.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    auto scopes = scopeFuture.get();
    ASSERT_TRUE(scopes);
    bool foundLocal = false;
    for (const auto& scope : *scopes) {
        auto variablesPromise = std::make_shared<std::promise<Result<VariableList>>>();
        auto variablesFuture = variablesPromise->get_future();
        (*session)->variables(scope.variablesReference, 0, 0,
                              [variablesPromise](auto answer) { variablesPromise->set_value(std::move(answer)); });
        ASSERT_EQ(variablesFuture.wait_for(std::chrono::seconds(20)), std::future_status::ready);
        auto variables = variablesFuture.get();
        ASSERT_TRUE(variables);
        for (const auto& variable : *variables)
            if (variable.name == "local") {
                EXPECT_EQ(variable.value, "42");
                foundLocal = true;
            }
    }
    EXPECT_TRUE(foundLocal);
    auto value = std::make_shared<std::promise<Result<dap::Variable>>>();
    auto result = value->get_future();
    (*session)->evaluate("local", frame->id, "watch", [value](auto answer) { value->set_value(std::move(answer)); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    auto local = result.get();
    ASSERT_TRUE(local) << local.error().describe();
    EXPECT_EQ(local->value, "42"); // fixture computes base(7) * 6 before its marked printf.
    ASSERT_TRUE((*session)->next());
    ASSERT_TRUE(test::waitUntil([&] {
        const auto selected = (*session)->selectedFrame();
        return selected && selected->line != line;
    }));
    ASSERT_TRUE((*session)->continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    ASSERT_TRUE((*session)->outcome());
    EXPECT_EQ((*session)->outcome()->exitCode, 0);
}
} // namespace frost::debug
