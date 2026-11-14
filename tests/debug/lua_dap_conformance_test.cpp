// SPDX-License-Identifier: Apache-2.0
// Historical catalogue preservation and refusal of the retired experimental Lua wire.
// Real VM conformance is mandatory in lua_dap_real_corpus_test.cpp. Historical expected failures
// are not acceptance criteria. No local peer supplies threads, frames, source or DAP replies.
#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/io/file_system.hpp"
#include "frost/rpc/json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#ifndef FROST_LUA_DAP_CONFORMANCE_BASELINE
#error "FROST_LUA_DAP_CONFORMANCE_BASELINE must identify the preserved historical catalogue"
#endif
namespace frost::debug {
namespace {
using namespace std::chrono_literals;
TEST(LuaDapLegacyRefusal, HistoricalCatalogueRetainsEveryScenarioIdentityWithoutAcceptingFailureVerdicts) {
    const auto bytes = io::realFileSystem().readFile(FROST_LUA_DAP_CONFORMANCE_BASELINE);
    ASSERT_TRUE(bytes) << bytes.error().describe();
    const auto catalogue = rpc::parseJson(*bytes);
    ASSERT_TRUE(catalogue) << catalogue.error().describe();
    const auto* scenarios = catalogue->find("scenarios");
    ASSERT_NE(scenarios, nullptr);
    ASSERT_TRUE(scenarios->isArray());
    const std::set<std::string> expected{"handshake.initialize",
                                         "handshake.attach_and_configuration_done",
                                         "threads.single_lua_thread",
                                         "execution.pause_and_stopped_event",
                                         "stack.stack_trace_while_stopped",
                                         "stack.stack_trace_paging",
                                         "source.loaded_source_retrieval",
                                         "execution.continue_and_continued_event",
                                         "lifecycle.owned_play_stop",
                                         "lifecycle.disconnect",
                                         "breakpoints.line",
                                         "breakpoints.multiple_lines",
                                         "breakpoints.conditional",
                                         "breakpoints.hit_condition",
                                         "breakpoints.logpoint",
                                         "breakpoints.pausepoint",
                                         "breakpoints.function",
                                         "breakpoints.exception",
                                         "stepping.step_over",
                                         "stepping.step_in",
                                         "stepping.step_out",
                                         "variables.scopes",
                                         "variables.locals",
                                         "variables.upvalues",
                                         "variables.tables",
                                         "variables.set_variable",
                                         "evaluate.watch",
                                         "evaluate.repl",
                                         "coroutines.multi_coroutine_threads",
                                         "lifecycle.terminate"};
    std::set<std::string> actual, categories;
    for (const auto& scenario : scenarios->items()) {
        EXPECT_TRUE(actual.emplace(scenario.stringAt("id")).second);
        EXPECT_FALSE(scenario.stringAt("category").empty());
        EXPECT_FALSE(scenario.stringAt("command").empty());
        categories.emplace(scenario.stringAt("category"));
    }
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(categories, (std::set<std::string>{"breakpoints", "coroutines", "evaluate", "execution", "handshake",
                                                 "lifecycle", "source", "stack", "stepping", "threads", "variables"}));
}
TEST(LuaDapLegacyRefusal, RetiredAdapterOverloadsProduceNoSyntheticInitializationOrThreadEvents) {
    std::atomic<unsigned> events = 0;
    DapHandlers handlers;
    handlers.onEvent = [&](std::string_view, const rpc::Json&) { ++events; };
    auto tokenFactory = luaDebugAdapterFactory(0, "retired-test-credential", 1s);
    auto token = tokenFactory(handlers);
    ASSERT_FALSE(token);
    EXPECT_EQ(token.error().code(), ErrorCode::Unsupported);
    auto streamFactory = luaDebugAdapterFactory(std::unique_ptr<io::LoopbackStream>{});
    auto stream = streamFactory(handlers);
    ASSERT_FALSE(stream);
    EXPECT_EQ(stream.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(events.load(), 0U);
}
TEST(LuaDapLegacyRefusal, RetiredEndpointOverloadsCannotGrantAConnectionGenerationOrOwnedPlayOperation) {
    std::uint64_t generation = 91;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    auto connection = connectLuaDebugEndpoint(0, "retired-test-credential", deadline, true, {}, &generation);
    ASSERT_FALSE(connection);
    EXPECT_EQ(connection.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(generation, 91U);
    auto owned = connectLuaDebugPlayEndpoint(0, "retired-test-credential", deadline);
    ASSERT_FALSE(owned);
    EXPECT_EQ(owned.error().code(), ErrorCode::Unsupported);
}
} // namespace
} // namespace frost::debug
