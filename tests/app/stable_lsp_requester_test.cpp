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

TEST(StableLspRequester, ManagedRequestsKeepTheirOwnServerAcrossTabSwitchAndReplacement) {
    frost::app::StableLspRequester requester;
    requester.setServer(std::make_shared<TestServer>(1));
    requester.setLuaServer(std::make_shared<TestServer>(2));
    auto managed = std::make_shared<TestServer>(3);
    std::weak_ptr<TestServer> old = managed;
    requester.setManagedServer(managed);
#ifdef _WIN32
    const std::string path = "C:\\project\\file.CS";
#else
    const std::string path = "/project/file.CS";
#endif
    auto params = frost::lang::Json::object().set(
        "textDocument", frost::lang::Json::object().set("uri", frost::lang::pathToUri(path).value()));
    requester.setActivePath(path);
    auto delayed = requester.request("textDocument/hover", params);
    managed.reset();
    requester.setManagedServer(std::make_shared<TestServer>(4));
    requester.setActivePath("plain.txt");
    ASSERT_FALSE(old.expired());
    auto answer = frost::sched::syncWait(std::move(delayed));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->asInteger(), 3);
    EXPECT_TRUE(old.expired());
    answer = frost::sched::syncWait(requester.request("textDocument/hover", params));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->asInteger(), 4);
}

TEST(StableLspRequester, FormattingRoutesAcrossMixedLanguageTabs) {
    frost::app::StableLspRequester requester;
    requester.setServer(std::make_shared<TestServer>(1));
    requester.setLuaServer(std::make_shared<TestServer>(2));
    requester.setManagedServer(std::make_shared<TestServer>(3));
#ifdef _WIN32
    const std::string root = "C:\\project\\";
#else
    const std::string root = "/project/";
#endif
    auto params = [](const std::string& path) {
        return frost::lang::Json::object().set(
            "textDocument", frost::lang::Json::object().set("uri", frost::lang::pathToUri(path).value()));
    };
    requester.setActivePath(root + "file.lua");
    auto lua = requester.request("textDocument/rangeFormatting", params(root + "file.lua"));
    requester.setActivePath(root + "file.cpp");
    auto cpp = requester.request("textDocument/formatting", params(root + "file.cpp"));
    requester.setActivePath(root + "file.cs");
    auto managed = requester.request("textDocument/formatting", params(root + "file.cs"));
    requester.setActivePath(root + "notes.txt");
    auto first = frost::sched::syncWait(std::move(lua));
    auto second = frost::sched::syncWait(std::move(cpp));
    auto third = frost::sched::syncWait(std::move(managed));
    ASSERT_TRUE(first && second && third);
    EXPECT_EQ(first->asInteger(), 2);
    EXPECT_EQ(second->asInteger(), 1);
    EXPECT_EQ(third->asInteger(), 3);

    // Workspace-wide requests without textDocument (e.g. workspace/symbol) still reach an available
    // language server even when a non-source tab (notes.txt) is active.
    auto sym = frost::sched::syncWait(
        requester.request("workspace/symbol", frost::lang::Json::object().set("query", "Player")));
    ASSERT_TRUE(sym.has_value());
    EXPECT_EQ(sym->asInteger(), 1);

    // Hierarchy requests carrying item.uri route by that item's language.
    auto itemParams = frost::lang::Json::object().set(
        "item", frost::lang::Json::object().set("uri", frost::lang::pathToUri(root + "file.cs").value()));
    auto hierarchy = frost::sched::syncWait(requester.request("typeHierarchy/supertypes", std::move(itemParams)));
    ASSERT_TRUE(hierarchy.has_value());
    EXPECT_EQ(hierarchy->asInteger(), 3);
}

TEST(StableLspRequester, OfflineJsonAndCMakeKeepTheirOwnRouteAcrossTabSwitch) {
    frost::app::StableLspRequester requester;
    requester.setServer(std::make_shared<TestServer>(1));
    requester.setJsonServer(std::make_shared<TestServer>(3));
    requester.setCMakeServer(std::make_shared<TestServer>(4));
#ifdef _WIN32
    const std::string base = "C:/project/";
#else
    const std::string base = "/project/";
#endif
    auto params = [&](std::string_view name) {
        return frost::lang::Json::object().set(
            "textDocument",
            frost::lang::Json::object().set("uri", frost::lang::pathToUri(base + std::string(name)).value()));
    };
    requester.setActivePath(base + "CMakePresets.json");
    auto json = requester.request("textDocument/completion", params("CMakePresets.json"));
    requester.setActivePath(base + "CMakeLists.txt");
    const auto cmake = frost::sched::syncWait(requester.request("textDocument/completion", params("CMakeLists.txt")));
    const auto jsonResult = frost::sched::syncWait(std::move(json));
    ASSERT_TRUE(jsonResult);
    ASSERT_TRUE(cmake);
    EXPECT_EQ(jsonResult->asInteger(), 3);
    EXPECT_EQ(cmake->asInteger(), 4);
    EXPECT_FALSE(frost::sched::syncWait(requester.request("textDocument/completion", params("arbitrary.json"))));
}

} // namespace
