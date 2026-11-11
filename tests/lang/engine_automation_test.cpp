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
        else if (method == "schema.export") {
            auto commands = Json::array();
            for (const char* name : {"play.start", "play.stop", "play.state", "test.run"})
                commands.push(Json::object().set("name", name).set("idePolicy", Json::object().set("allowed", true)));
            result.set("commands", std::move(commands));
        } else if (method == "project.info") {
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
    return [script](std::uint16_t, auto, const std::stop_token&) -> Result<std::unique_ptr<IEngineConnection>> {
        return std::unique_ptr<IEngineConnection>(new Fake(script));
    };
}
auto same = [](std::string_view a, std::string_view b) { return a == b; };
auto trusted = [] { return true; };
auto deadline() { return std::chrono::steady_clock::now() + std::chrono::seconds(2); }

// A test policy receives the real scripted schema reply. Full schema-policy behaviour is covered
// in engine_tests; this layer verifies callback ordering and never depends on the engine layer.
Result<Json> runAdmittedCommand(const Json& discoveryDoc, std::string_view projectRoot, std::string_view method,
                                Json params, const std::function<bool(std::string_view, std::string_view)>& sameProject,
                                const std::function<bool()>& stillTrusted, const EngineConnect& connect,
                                std::chrono::steady_clock::time_point expires, std::stop_token stop = {}) {
    return runEngineCommand(
        discoveryDoc, projectRoot, method, std::move(params), sameProject, stillTrusted, connect, expires, stop,
        [](std::string_view name, const Json&, const Json& schema, const Json& server,
           const Json& project) -> Result<void> {
            EXPECT_EQ(server.integerAt("contractVersion"), 1);
            EXPECT_EQ(project.stringAt("directory"), kRoot);
            const auto* commands = schema.find("commands");
            if (commands && commands->isArray()) {
                for (const Json& command : commands->items()) {
                    const Json* policy = command.find("idePolicy");
                    if (command.stringAt("name") == name && policy && policy->boolAt("allowed") == true)
                        return {};
                }
            }
            return fail(ErrorCode::PermissionDenied, "command has no schema opt-in");
        });
}

TEST(EngineAutomation, MissingPolicyNeverConnects) {
    auto script = std::make_shared<Script>();
    auto result = runEngineCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                   deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(script->methods.empty());
}

TEST(EngineAutomation, AdmissionRefusalAndTrustRevocationInsideAdmissionNeverDispatch) {
    auto script = std::make_shared<Script>();
    bool permission = true;
    bool refuse = true;
    const EngineCommandAdmission admission = [&](std::string_view method, const Json& params, const Json& schema,
                                                 const Json& server, const Json& project) -> Result<void> {
        EXPECT_EQ(method, "play.start");
        EXPECT_TRUE(params.isObject());
        EXPECT_TRUE(schema.find("commands"));
        EXPECT_EQ(server.integerAt("contractVersion"), 1);
        EXPECT_EQ(project.stringAt("directory"), kRoot);
        if (refuse)
            return fail(ErrorCode::PermissionDenied, "scripted admission refusal");
        permission = false;
        return {};
    };
    auto result = runEngineCommand(
        discovery(), kRoot, "play.start", Json::object(), same, [&] { return permission; }, connector(script),
        deadline(), {}, admission);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(script->methods.back(), "schema.export");
    refuse = false;
    script->methods.clear();
    result = runEngineCommand(
        discovery(), kRoot, "play.start", Json::object(), same, [&] { return permission; }, connector(script),
        deadline(), {}, admission);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(script->methods.back(), "schema.export");
}

TEST(EngineAutomation, AuthenticatesAndChecksLiveProjectBeforeCommand) {
    auto script = std::make_shared<Script>();
    auto result = runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                     deadline());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->stringAt("state"), "Play");
    EXPECT_EQ(script->methods,
              (std::vector<std::string>{"auth", "server.info", "project.info", "schema.export", "play.start"}));
}
TEST(EngineAutomation, RefusesWrongLiveProjectAndRevokedTrustBeforeCommand) {
    auto script = std::make_shared<Script>();
    script->liveRoot = kOther;
    auto result = runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                     deadline());
    EXPECT_FALSE(result);
    EXPECT_EQ(script->methods.back(), "project.info");
    script->methods.clear();
    script->liveRoot = kRoot;
    bool permission = true;
    script->afterProject = [&permission] { permission = false; };
    result = runAdmittedCommand(
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
        runAdmittedCommand(doc, kRoot, "play.start", Json::object(), same, trusted, connector(script), deadline()));
    EXPECT_FALSE(runAdmittedCommand(discovery(), kRoot, "server.shutdown", Json::object(), same, trusted,
                                    connector(script), deadline()));
    EXPECT_EQ(script->methods.back(), "schema.export");
    script->methods.clear();
    script->malformed = true;
    EXPECT_FALSE(runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                    deadline()));
    EXPECT_EQ(script->methods.size(), 1);
}

TEST(EngineAutomation, RefusesLuaDebugMethodsBecauseFerEightOwnsThem) {
    // script.debug.* is FER-8's (Lua debugging through FrostEngine); nothing may reach the engine.
    auto script = std::make_shared<Script>();
    for (const char* method : {"script.debug.start", "script.debug.play", "script.debug.stop"}) {
        auto result = runAdmittedCommand(discovery(), kRoot, method, Json::object(), same, trusted, connector(script),
                                         deadline());
        ASSERT_FALSE(result) << method;
        EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied) << method;
        EXPECT_EQ(script->methods.back(), "schema.export");
    }
    for (const std::string& method : script->methods)
        EXPECT_FALSE(method.starts_with("script.debug."));
}

TEST(EngineAutomation, ReportsTheEngineErrorCodeAndRedactedMessage) {
    auto script = std::make_shared<Script>();
    script->rejectMethod = "play.start";
    script->rejectMessage = "Play refused: token secret is stale\n(editor busy)";
    auto result = runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                     deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    const std::string message(result.error().message());
    EXPECT_NE(message.find("-32000"), std::string::npos) << message;
    EXPECT_NE(message.find("Play refused: token <redacted> is stale (editor busy)"), std::string::npos) << message;
    EXPECT_EQ(message.find("secret"), std::string::npos) << message;
    script->rejectCode = -32601;
    result = runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(std::string(result.error().message()).find("-32601"), std::string::npos);
}

TEST(EngineAutomation, RedactsAndBoundsEngineMessages) {
    EXPECT_EQ(redactEngineMessage("abc secret def secret", "secret"), "abc <redacted> def <redacted>");
    EXPECT_EQ(redactEngineMessage("tab\there", ""), "tab here");
    // Bidi overrides, isolates, marks and the Arabic letter mark become spaces; other text stays.
    // NOLINTNEXTLINE(misc-misleading-bidirectional): Deliberately tests that bidi control bytes are redacted.
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
    auto result = runAdmittedCommand(discovery(), kRoot, "test.run", Json::object(), same, trusted, connector(script),
                                     std::chrono::steady_clock::time_point::max(), cancel.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(script->methods.empty());
}

TEST(EngineAutomation, CancellationBeforeDispatchNeverConnects) {
    auto script = std::make_shared<Script>();
    std::stop_source cancel;
    cancel.request_stop();
    auto result = runAdmittedCommand(discovery(), kRoot, "play.start", Json::object(), same, trusted, connector(script),
                                     deadline(), cancel.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(script->methods.empty());
}
TEST(EngineAutomation, DoesNotReplayMutatingCommandAfterTimeout) {
    auto script = std::make_shared<Script>();
    script->timeoutMethod = "test.run";
    auto result = runAdmittedCommand(discovery(), kRoot, "test.run", Json::object(), same, trusted, connector(script),
                                     deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(script->methods,
              (std::vector<std::string>{"auth", "server.info", "project.info", "schema.export", "test.run"}));
}
} // namespace
