// SPDX-License-Identifier: Apache-2.0
// The workbench's replaceable requester retains the server captured by a delayed request.

#include "frost/sched/scheduler.hpp"

#include "../../src/app/src/stable_lsp_requester.hpp"
#include <gtest/gtest.h>

#include <memory>

namespace {

class TestServer final : public frost::lang::ILspRequester {
public:
    explicit TestServer(int identity) : m_identity(identity) {}
    frost::sched::Task<frost::Result<frost::lang::Json>> request(std::string, frost::lang::Json,
                                                                 std::stop_token) override {
        co_return frost::lang::Json(m_identity);
    }
    frost::lang::PositionEncoding encoding() const override { return frost::lang::PositionEncoding::Utf16; }
    std::optional<frost::lang::ServerCapabilities> serverCapabilities() const override { return std::nullopt; }

private:
    int m_identity;
};

TEST(StableLspRequester, DelayedRequestRetainsOldServerAcrossReplacementAndClose) {
    frost::app::StableLspRequester requester;
    auto first = std::make_shared<TestServer>(1);
    std::weak_ptr<TestServer> old = first;
    requester.setServer(first);
    auto delayed = requester.request("semantic", frost::lang::Json::object());
    first.reset();
    requester.setServer(std::make_shared<TestServer>(2));
    EXPECT_FALSE(old.expired());
    auto result = frost::sched::syncWait(std::move(delayed));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->asInteger(), 1);
    EXPECT_TRUE(old.expired());

    auto afterRestart = requester.request("semantic", frost::lang::Json::object());
    requester.setServer(nullptr);
    result = frost::sched::syncWait(std::move(afterRestart));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->asInteger(), 2);
    result = frost::sched::syncWait(requester.request("semantic", frost::lang::Json::object()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Busy);
}

TEST(StableLspRequester, CancelledDelayedRequestDoesNotReachReplacement) {
    frost::app::StableLspRequester requester;
    requester.setServer(std::make_shared<TestServer>(1));
    std::stop_source stop;
    auto delayed = requester.request("semantic", frost::lang::Json::object(), stop.get_token());
    requester.setServer(std::make_shared<TestServer>(2));
    stop.request_stop();
    auto result = frost::sched::syncWait(std::move(delayed));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
}

TEST(StableLspRequester, RoutesByDocumentAcrossTabSwitchAndClose) {
    frost::app::StableLspRequester requester;
    requester.setServer(std::make_shared<TestServer>(1));
    requester.setLuaServer(std::make_shared<TestServer>(2));
#ifdef _WIN32
    const std::string lua = "C:\\project\\test.lua";
    const std::string cpp = "C:\\project\\test.cpp";
#else
    const std::string lua = "/project/test.lua";
    const std::string cpp = "/project/test.cpp";
#endif
    auto params = [](const std::string& path) {
        return frost::lang::Json::object().set(
            "textDocument", frost::lang::Json::object().set("uri", frost::lang::pathToUri(path).value()));
    };
    requester.setActivePath(lua);
    auto delayed = requester.request("textDocument/hover", params(lua));
    requester.setActivePath(cpp);
    auto cppRequest = requester.request("textDocument/hover", params(cpp));
    requester.setLuaServer(nullptr);
    auto luaResult = frost::sched::syncWait(std::move(delayed));
    auto cppResult = frost::sched::syncWait(std::move(cppRequest));
    ASSERT_TRUE(luaResult.has_value());
    ASSERT_TRUE(cppResult.has_value());
    EXPECT_EQ(luaResult->asInteger(), 2);
    EXPECT_EQ(cppResult->asInteger(), 1);
    EXPECT_FALSE(frost::sched::syncWait(requester.request("textDocument/hover", params(lua))).has_value());
}

} // namespace
