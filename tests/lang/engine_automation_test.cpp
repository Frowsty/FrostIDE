// SPDX-License-Identifier: Apache-2.0
// Tests for FrostEngine automation discovery and request validation.
#include "frost/lang/engine_automation.hpp"

#include <gtest/gtest.h>

#include <memory>
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
    std::function<void()> afterProject;
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
