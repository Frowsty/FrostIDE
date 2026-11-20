// SPDX-License-Identifier: Apache-2.0
// Synthetic offline protocol fixtures are not claims about observed unmodified CLI behavior.
#include "frost/agent/protocol.hpp"
#include <gtest/gtest.h>
#include <algorithm>
using namespace std::chrono_literals;
namespace {
using namespace frost::agent;
Json auth(std::string method) { return Json::object().set("loggedIn", true).set("authMethod", std::move(method)); }
NativeLaunch launch() {
    NativeLaunch value;
#ifdef _WIN32
    value.executable = "C:\\installed\\claude.exe"; value.root = "C:\\workspace"; value.settingsFile = "C:\\private\\hooks.json"; value.mcpConfig = "C:\\private\\mcp.json";
#else
    value.executable = "/opt/claude"; value.root = "/workspace"; value.settingsFile = "/private/hooks.json"; value.mcpConfig = "/private/mcp.json";
#endif
    value.idePort = 45678;
    return value;
}
TEST(NativeAuth, ProductionGateRefusesSubscriptionUnknownAndLoggedOutEveryTime) {
    static_assert(!kSubscriptionApprovalRecorded);
    for (const auto* method : {"claude.ai", "oauth_token", "subscription", "unknown"}) EXPECT_FALSE(nativeLaunch(launch(), auth(method)));
    EXPECT_FALSE(nativeLaunch(launch(), Json::object().set("loggedIn", false).set("authMethod", "api_key")));
    ASSERT_TRUE(nativeLaunch(launch(), auth("api_key")));
    EXPECT_FALSE(nativeLaunch(launch(), auth("subscription"))); // No cached authorization survives the next session.
}
TEST(NativeAuth, AllAcceptedNonSubscriptionLaunchesCarryExplicitIntegrationAndBare) {
    for (const auto* method : {"api_key", "apiKeyHelper", "bedrock", "vertex"}) {
        const auto planned = nativeLaunch(launch(), auth(method));
        ASSERT_TRUE(planned);
        for (const auto* flag : {"--bare", "--ide", "--settings", "--mcp-config", "--strict-mcp-config", "--permission-prompt-tool", "--input-format", "--output-format"})
            EXPECT_NE(std::ranges::find(planned->arguments, flag), planned->arguments.end());
        EXPECT_EQ(std::ranges::find(planned->arguments, "--dangerously-skip-permissions"), planned->arguments.end());
        EXPECT_NE(std::ranges::find(planned->arguments, "AskUserQuestion,ExitPlanMode"), planned->arguments.end());
        EXPECT_EQ(planned->workingDirectory, launch().root);
        EXPECT_EQ(planned->stdinMode, frost::io::Stdio::Pipe); EXPECT_EQ(planned->stdoutMode, frost::io::Stdio::Pipe); EXPECT_EQ(planned->stderrMode, frost::io::Stdio::Pipe);
    }
    auto invalid = launch(); invalid.model = "--dangerously-skip-permissions"; EXPECT_FALSE(nativeLaunch(invalid, auth("api_key")));
    invalid = launch(); invalid.idePort = 0; EXPECT_FALSE(nativeLaunch(invalid, auth("api_key")));
    invalid = launch(); invalid.forkSession = true; EXPECT_FALSE(nativeLaunch(invalid, auth("api_key")));
}
TEST(NativeStream, HandlesSplitEscapedAndCrLfEventsWithoutInventingPartialMessages) {
    NdjsonFramer framer;
    auto first = framer.feed("{\"type\":\"assistant\",\"text\":\"line\\n"); ASSERT_TRUE(first); EXPECT_TRUE(first->empty());
    auto second = framer.feed("two\"}\r\n{\"type\":\"result\"}\n"); ASSERT_TRUE(second); ASSERT_EQ(second->size(), 2);
    EXPECT_EQ(second->front().stringAt("text"), "line\ntwo"); EXPECT_TRUE(framer.finish());
}
TEST(NativeStream, OversizedMalformedAndTruncatedInputFailClosed) {
    NdjsonFramer huge; EXPECT_FALSE(huge.feed(std::string(NdjsonFramer::kMaximumLine + 1, 'x'))); EXPECT_FALSE(huge.feed("{}\n"));
    NdjsonFramer malformed; EXPECT_FALSE(malformed.feed("not-json\n")); EXPECT_FALSE(malformed.feed("{}\n"));
    NdjsonFramer partial; ASSERT_TRUE(partial.feed("{\"type\":")); EXPECT_FALSE(partial.finish());
}
Json initialized() {
    return Json::object().set("type", "system").set("subtype", "init").set("claude_code_version", "2.1.316").set("session_id", "fixture-session")
        .set("ide_connected", true).set("mcp_servers", Json::array().push(Json::object().set("name", "frostide").set("status", "connected")))
        .set("supported_control_requests", Json::array().push("interrupt").push("set_model").push("get_context_usage"));
}
TEST(NativeProtocol, UserTurnsWaitForActualCompatibleIdeAndMcpInit) {
    NativeProtocol protocol; EXPECT_FALSE(protocol.userTurn("hello"));
    auto missing = initialized(); missing.set("ide_connected", false); ASSERT_TRUE(protocol.receive(missing)); EXPECT_FALSE(protocol.ready());
    NativeProtocol ready; ASSERT_TRUE(ready.receive(initialized())); EXPECT_TRUE(ready.ready());
    auto message = ready.userTurn("explicit user turn"); ASSERT_TRUE(message); EXPECT_EQ(message->stringAt("type"), "user");
    EXPECT_FALSE(ready.supports("rewind_files")); EXPECT_TRUE(ready.supports("interrupt"));
    NativeProtocol old; auto outdated = initialized(); outdated.set("claude_code_version", "2.1.315"); EXPECT_FALSE(old.receive(outdated));
}
TEST(NativeProtocol, ControlsHaveCorrelatedSixtySecondTimeoutAndCancellation) {
    NativeProtocol protocol; const auto now = NativeProtocol::Time{};
    auto initialize = protocol.control("initialize", Json::object(), now); ASSERT_TRUE(initialize);
    EXPECT_TRUE(protocol.expire(now + 59s).empty());
    auto cancelled = protocol.expire(now + 60s); ASSERT_EQ(cancelled.size(), 1);
    EXPECT_EQ(cancelled[0].stringAt("request_id"), initialize->stringAt("request_id")); EXPECT_EQ(cancelled[0].stringAt("type"), "control_cancel_request");
    EXPECT_TRUE(protocol.expire(now + 120s).empty());
    EXPECT_FALSE(protocol.receive(Json::object().set("type", "control_response").set("response", Json::object().set("request_id", std::string(initialize->stringAt("request_id"))))));
}
TEST(McpLimits, PagesAreBoundedAndNoRecordIsSilentlyTruncated) {
    auto result = Json::array(); for (int i = 0; i < 100; ++i) result.push(Json::object().set("id", i).set("text", std::string(400, 'x')));
    auto pages = paginate(result); ASSERT_TRUE(pages); EXPECT_GT(pages->size(), 1);
    std::size_t records = 0; for (const auto& page : *pages) { EXPECT_LE(page.dump().size(), 12000); records += page.items().size(); }
    EXPECT_EQ(records, 100); EXPECT_FALSE(paginate(Json::object().set("text", std::string(13000, 'x'))));
    EXPECT_FALSE(paginate(Json::array().push(Json(std::string(13000, 'x')))));
}
TEST(McpLimits, FourAlwaysLoadedToolsAndEveryMutationRequiresInteraction) {
    const auto catalog = mcpToolCatalog(); unsigned always = 0;
    for (const auto& tool : catalog.find("tools")->items()) {
        const auto* meta = tool.find("_meta"); ASSERT_NE(meta, nullptr); if (meta->boolAt("anthropic/alwaysLoad").value_or(false)) ++always;
        if (!tool.find("annotations")->boolAt("readOnlyHint").value_or(true)) EXPECT_TRUE(meta->boolAt("anthropic/requiresUserInteraction").value_or(false));
    }
    EXPECT_EQ(always, 4);
}
TEST(StopHooks, RealErrorSetBlocksAtMostThreeTimesAndNestedStopNeverRecurses) {
    StopHookGuard guard;
    EXPECT_FALSE(guard.evaluate("a", "error", true).find("decision"));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(std::string(guard.evaluate("a", "error", false).stringAt("decision")), "block");
    EXPECT_TRUE(guard.evaluate("a", "error", false).find("warning"));
    EXPECT_EQ(std::string(guard.evaluate("b", "new error", false).stringAt("decision")), "block");
}
}
