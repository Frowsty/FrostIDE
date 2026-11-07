// SPDX-License-Identifier: Apache-2.0
// Tests for FrostEngine automation discovery and request validation.
#include "frost/lang/engine_automation.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace {
#ifdef _WIN32
constexpr auto kRoot = "C:/game";
constexpr auto kOther = "C:/other";
#else
constexpr auto kRoot = "/game";
constexpr auto kOther = "/other";
#endif
using namespace frost;
using namespace frost::lang;
struct Script {
    std::vector<std::string> methods;
    std::string liveRoot = kRoot;
    bool malformed = false;
    std::string timeoutMethod;
    std::string rejectMethod;
    std::string ioFailureMethod;
    std::function<void()> afterProject;
    std::int64_t rejectCode = -32000;
    std::string rejectMessage;
};
class Fake final : public IEngineConnection {
public:
    explicit Fake(std::shared_ptr<Script> script) : m_script(std::move(script)) {}
    Result<std::string> exchange(std::string_view bytes) override {
        auto request = parseJson(bytes);
        if (!request)
            return fail(ErrorCode::Corrupt, "bad request");
        const auto method = request->stringAt("method");
        m_script->methods.emplace_back(method);
        if (method == m_script->timeoutMethod)
            return fail(ErrorCode::Timeout, "simulated timeout");
        if (method == m_script->ioFailureMethod)
            return fail(ErrorCode::IoError, "simulated disconnect");
        if (method == m_script->rejectMethod)
            return Json::object()
                .set("jsonrpc", "2.0")
                .set("id", *request->integerAt("id"))
                .set("error", Json::object().set("code", m_script->rejectCode).set("message", m_script->rejectMessage))
                .dump();
        auto result = Json::object();
        if (method == "auth")
            result.set("authenticated", request->find("params")->stringAt("token") == "secret");
        else if (method == "server.info")
            result.set("contractVersion", 1);
        else if (method == "project.info") {
            result.set("directory", m_script->liveRoot);
            if (m_script->afterProject)
                m_script->afterProject();
        } else
            result.set("state", "Play");
        if (m_script->malformed)
            return std::string("not json");
        return Json::object()
            .set("jsonrpc", "2.0")
            .set("id", *request->integerAt("id"))
            .set("result", std::move(result))
            .dump();
    }

private:
    std::shared_ptr<Script> m_script;
};
Json discovery() {
    return Json::object()
        .set("host", "127.0.0.1")
        .set("port", 9010)
        .set("tokenRequired", true)
        .set("token", "secret")
        .set("projectPath", kRoot);
}
auto connector(const std::shared_ptr<Script>& script) {
    return [script](std::uint16_t, auto, std::stop_token) -> Result<std::unique_ptr<IEngineConnection>> {
        return std::unique_ptr<IEngineConnection>(new Fake(script));
    };
}
auto same = [](std::string_view a, std::string_view b) { return a == b; };
auto trusted = [] { return true; };
auto deadline() { return std::chrono::steady_clock::now() + std::chrono::seconds(2); }

TEST(EngineAutomation, AuthenticatesAndChecksLiveProjectBeforeCommand) {
    auto script = std::make_shared<Script>();
    auto result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                   deadline());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->stringAt("state"), "Play");
    EXPECT_EQ(script->methods, (std::vector<std::string>{"auth", "server.info", "project.info", "play.start"}));
}
TEST(EngineAutomation, RefusesWrongLiveProjectAndRevokedTrustBeforeCommand) {
    auto script = std::make_shared<Script>();
    script->liveRoot = kOther;
    auto result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                   deadline());
    EXPECT_FALSE(result);
    EXPECT_EQ(script->methods.back(), "project.info");
    script->methods.clear();
    script->liveRoot = kRoot;
    bool permission = true;
    script->afterProject = [&permission] { permission = false; };
    result = runEngineCommand(
        discovery(), kRoot, "play.stop", Json::object(), same, [&] { return permission; }, connector(script),
        deadline());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(script->methods.back(), "project.info");
}
TEST(EngineAutomation, RefusesRemoteHostUnknownCommandAndMalformedReply) {
    auto script = std::make_shared<Script>();
    auto doc = discovery();
    doc.set("host", "192.0.2.1");
    EXPECT_FALSE(
        runEngineCommand(doc, kRoot, "play.start", Json::object(), same, trusted, connector(script), deadline()));
    EXPECT_FALSE(runEngineCommand(discovery(), kRoot, "server.shutdown", Json::object(), same, trusted,
                                  connector(script), deadline()));
    EXPECT_TRUE(script->methods.empty());
    script->malformed = true;
    EXPECT_FALSE(runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                  deadline()));
    EXPECT_EQ(script->methods.size(), 1);
}

TEST(EngineAutomation, RefusesLuaDebugMethodsBecauseFerEightOwnsThem) {
    // script.debug.* is FER-8's (Lua debugging through FrostEngine); nothing may reach the engine.
    auto script = std::make_shared<Script>();
    for (const char* method : {"script.debug.start", "script.debug.play", "script.debug.stop"}) {
        auto result =
            runEngineCommand(discovery(), kRoot, method, Json::object(), same, trusted, connector(script), deadline());
        ASSERT_FALSE(result) << method;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << method;
    }
    EXPECT_TRUE(script->methods.empty());
}

TEST(EngineAutomation, ReportsTheEngineErrorCodeAndRedactedMessage) {
    auto script = std::make_shared<Script>();
    script->rejectMethod = "play.start";
    script->rejectMessage = "Play refused: token secret is stale\n(editor busy)";
    auto result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                   deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    const std::string message(result.error().message());
    EXPECT_NE(message.find("-32000"), std::string::npos) << message;
    EXPECT_NE(message.find("Play refused: token <redacted> is stale (editor busy)"), std::string::npos) << message;
    EXPECT_EQ(message.find("secret"), std::string::npos) << message;
    script->rejectCode = -32601;
    result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                              deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(std::string(result.error().message()).find("-32601"), std::string::npos);
}

TEST(EngineAutomation, RedactsAndBoundsEngineMessages) {
    EXPECT_EQ(redactEngineMessage("abc secret def secret", "secret"), "abc <redacted> def <redacted>");
    EXPECT_EQ(redactEngineMessage("tab\there", ""), "tab here");
    // Bidi overrides, isolates, marks and the Arabic letter mark become spaces; other text stays.
    EXPECT_EQ(redactEngineMessage("a\xE2\x80\xAE"
                                  "b\xE2\x81\xA6"
                                  "c\xE2\x80\x8F"
                                  "d\xD8\x9C"
                                  "e",
                                  ""),
              "a b c d e");
    EXPECT_EQ(redactEngineMessage("caf\xC3\xA9 \xE2\x80\x94 ok", ""), "caf\xC3\xA9 \xE2\x80\x94 ok");
    const std::string bounded = redactEngineMessage(std::string(4 * kEngineMessageLimit, 'x'), "secret");
    EXPECT_EQ(bounded.size(), kEngineMessageLimit + 3);
    EXPECT_TRUE(bounded.ends_with("..."));
    // "a" then two-byte sequences: the limit falls inside one, and the cut moves back before it.
    std::string accented = "a";
    while (accented.size() < 2 * kEngineMessageLimit)
        accented += "\xC3\xA9";
    const std::string cut = redactEngineMessage(accented, {});
    ASSERT_TRUE(cut.ends_with("..."));
    const std::string body = cut.substr(0, cut.size() - 3);
    EXPECT_EQ(body.size(), kEngineMessageLimit - 1);
    EXPECT_EQ(body.back(), '\xA9');
}

TEST(EngineAutomation, PerMethodDeadlines) {
    // test.run is unbounded (cancellable); play.* and log.get are short; script.validate in between.
    EXPECT_FALSE(engineMethodTimeout("test.run").has_value());
    for (const char* method : {"play.start", "play.stop", "play.state", "log.get"}) {
        ASSERT_TRUE(engineMethodTimeout(method).has_value()) << method;
        EXPECT_LE(*engineMethodTimeout(method), std::chrono::seconds(10)) << method;
    }
    ASSERT_TRUE(engineMethodTimeout("script.validate").has_value());
    EXPECT_GE(*engineMethodTimeout("script.validate"), *engineMethodTimeout("play.start"));
}

TEST(EngineAutomation, UnboundedTestRunStillStopsOnCancel) {
    auto script = std::make_shared<Script>();
    std::stop_source cancel;
    cancel.request_stop();
    auto result = runEngineCommand(discovery(), kRoot, "test.run", Json::object(), same, trusted, connector(script),
                                   std::chrono::steady_clock::time_point::max(), cancel.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(script->methods.empty());
}

TEST(EngineAutomation, CancellationBeforeDispatchNeverConnects) {
    auto script = std::make_shared<Script>();
    std::stop_source cancel;
    cancel.request_stop();
    auto result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                   deadline(), cancel.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(script->methods.empty());
}
TEST(EngineAutomation, DoesNotReplayMutatingCommandAfterTimeout) {
    auto script = std::make_shared<Script>();
    script->timeoutMethod = "test.run";
    auto result =
        runEngineCommand(discovery(), kRoot, "test.run", Json::object(), same, trusted, connector(script), deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(script->methods, (std::vector<std::string>{"auth", "server.info", "project.info", "test.run"}));
}
} // namespace
