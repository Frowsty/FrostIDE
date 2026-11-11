// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the workspace side of LspClient (lsp_client.hpp, LUA-3) against the scripted
// in-memory server: multi-root workspaceFolders in initialize and as the answer to
// workspace/workspaceFolders, workspace/configuration answered per scopeUri, the extra workspace
// capabilities a client announces, watched-files registrations kept and dropped, refresh requests
// answered and passed on, and the glob matching of fileWatcherMatches.

#include "frost/lang/lsp_client.hpp"
#include "frost/lang/uri.hpp"

#include "lang/fake_server.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace {

using frost::lang::FileChangeType;
using frost::lang::FileSystemWatcher;
using frost::lang::InitializeOptions;
using frost::lang::Json;
using frost::lang::LspClient;
using frost::test::eventually;
using frost::test::FakeServer;
using frost::test::LangEnv;
using frost::test::MemoryPipe;

#ifdef _WIN32
const std::string& kRoot() {
    static const std::string value = "C:\\work\\proj";
    return value;
}
const std::string& kGame() {
    static const std::string value = "C:\\work\\proj\\Samples\\Game";
    return value;
}
#else
const std::string& kRoot() {
    static const std::string value = "/work/proj";
    return value;
}
const std::string& kGame() {
    static const std::string value = "/work/proj/Samples/Game";
    return value;
}
#endif

std::string uri(const std::string& path) { return frost::lang::pathToUri(path).value(); }

InitializeOptions multiRoot() {
    InitializeOptions init;
    init.rootPath = kRoot();
    init.workspaceFolders = {kGame(), kRoot()};
    init.scopedConfiguration = [](std::string_view scopeUri, std::string_view section) {
        return Json(std::string(section) + "@" + (scopeUri.empty() ? std::string("none") : std::string(scopeUri)));
    };
    init.workspaceCapabilities = Json::object()
                                     .set("semanticTokens", Json::object().set("refreshSupport", true))
                                     .set("didChangeWatchedFiles", Json::object().set("dynamicRegistration", true));
    return init;
}

TEST(LspWorkspace, InitializeAnnouncesEveryFolderInOrderAndTheExtraCapabilities) {
    const auto params = frost::lang::initializeParams(multiRoot());
    ASSERT_TRUE(params.has_value()) << params.error().describe();
    const Json* folders = params->find("workspaceFolders");
    ASSERT_NE(folders, nullptr);
    ASSERT_EQ(folders->size(), 2U);
    EXPECT_EQ(folders->items()[0].stringAt("uri"), uri(kGame()));
    EXPECT_EQ(folders->items()[0].stringAt("name"), "Game");
    EXPECT_EQ(folders->items()[1].stringAt("uri"), uri(kRoot()));
    EXPECT_EQ(params->stringAt("rootUri"), uri(kRoot()));
    const Json* workspace = params->find("capabilities")->find("workspace");
    ASSERT_NE(workspace, nullptr);
    EXPECT_EQ(workspace->find("semanticTokens")->boolAt("refreshSupport"), true);
    EXPECT_EQ(workspace->find("didChangeWatchedFiles")->boolAt("dynamicRegistration"), true);
    // The common capabilities stay.
    EXPECT_EQ(workspace->boolAt("configuration"), true);
    EXPECT_EQ(workspace->find("inlayHint")->boolAt("refreshSupport"), true);

    // Without workspaceFolders the root alone is the folder, and nothing extra is announced.
    InitializeOptions plain;
    plain.rootPath = kRoot();
    const auto single = frost::lang::initializeParams(plain);
    ASSERT_TRUE(single.has_value());
    ASSERT_EQ(single->find("workspaceFolders")->size(), 1U);
    EXPECT_EQ(single->find("workspaceFolders")->items()[0].stringAt("uri"), uri(kRoot()));
    EXPECT_EQ(single->find("capabilities")->find("workspace")->find("semanticTokens")->boolAt("refreshSupport"), false);
    EXPECT_EQ(single->find("capabilities")->find("workspace")->find("didChangeWatchedFiles"), nullptr);
}

TEST(LspWorkspace, AnswersFoldersScopedConfigurationRegistrationsAndRefreshes) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    const Json watchers = Json::object().set(
        "watchers", Json::array({Json::object().set("globPattern", "**/*.lua").set("kind", 1 | 4),
                                 Json::object().set(
                                     "globPattern",
                                     Json::object().set("baseUri", uri(kGame())).set("pattern", "Assets/**/*.json"))}));
    FakeServer server(pipe, [&](FakeServer& s, const Json& message) {
        const std::string_view method = message.stringAt("method");
        if (method == "initialize") {
            s.respond(*message.find("id"), Json::object().set("capabilities", Json::object()));
        } else if (method == "initialized") {
            s.requestClient(1, "workspace/workspaceFolders", Json());
            s.requestClient(
                2, "workspace/configuration",
                Json::object().set("items",
                                   Json::array({Json::object().set("scopeUri", uri(kGame())).set("section", "Lua"),
                                                Json::object().set("section", "Lua.runtime")})));
            s.requestClient(
                3, "client/registerCapability",
                Json::object().set(
                    "registrations",
                    Json::array({Json::object()
                                     .set("id", "w1")
                                     .set("method", "workspace/didChangeWatchedFiles")
                                     .set("registerOptions", watchers),
                                 Json::object().set("id", "other").set("method", "textDocument/formatting")})));
            s.requestClient(4, "workspace/semanticTokens/refresh", Json());
        } else if (method == "frost/unregister") {
            // LuaLS sends its unregistration through registerCapability with "unregisterations".
            s.requestClient(5, "client/registerCapability",
                            Json::object().set("unregisterations", Json::array({Json::object().set("id", "w1")})));
        }
        return true;
    });
    struct Seen {
        std::mutex mutex;
        std::vector<std::string> refreshes;
    };
    auto seen = std::make_shared<Seen>();
    LspClient::Handlers handlers;
    handlers.onRefresh = [seen](std::string_view method) {
        const std::scoped_lock lock(seen->mutex);
        seen->refreshes.emplace_back(method);
    };
    LspClient client(pipe.clientStream(), env.pool(), multiRoot(), std::move(handlers));
    ASSERT_TRUE(env.run(client.initialize()).has_value());
    ASSERT_TRUE(server.waitForMessages(6)); // initialize, initialized and four answers.
    const auto received = server.received();
    const auto answerTo = [&](std::int64_t id) -> const Json* {
        for (const Json& message : received)
            if (message.integerAt("id") == id && message.find("method") == nullptr)
                return message.find("result");
        return nullptr;
    };
    const Json* folders = answerTo(1);
    ASSERT_NE(folders, nullptr);
    ASSERT_EQ(folders->size(), 2U);
    EXPECT_EQ(folders->items()[0].stringAt("uri"), uri(kGame()));
    const Json* configuration = answerTo(2);
    ASSERT_NE(configuration, nullptr);
    EXPECT_EQ(configuration->dump(), Json::array({Json("Lua@" + uri(kGame())), Json("Lua.runtime@none")}).dump());
    ASSERT_NE(answerTo(3), nullptr);
    EXPECT_TRUE(answerTo(3)->isNull());
    ASSERT_NE(answerTo(4), nullptr);
    EXPECT_TRUE(answerTo(4)->isNull());
    EXPECT_TRUE(eventually([&] {
        const std::scoped_lock lock(seen->mutex);
        return seen->refreshes == std::vector<std::string>{"workspace/semanticTokens/refresh"};
    }));

    // Only the watched-files registration is kept, with both pattern forms.
    const auto kept = client.fileWatchers();
    ASSERT_EQ(kept.size(), 2U);
    EXPECT_EQ(kept[0].glob, "**/*.lua");
    EXPECT_EQ(kept[0].kind, 5);
    EXPECT_TRUE(kept[0].basePath.empty());
    EXPECT_EQ(kept[1].glob, "Assets/**/*.json");
    EXPECT_EQ(kept[1].basePath, kGame());
    EXPECT_EQ(kept[1].kind, 7);

    ASSERT_TRUE(client.notify("frost/unregister", Json::object()).has_value());
    EXPECT_TRUE(eventually([&] { return client.fileWatchers().empty(); }));
}

TEST(LspWorkspace, WatchersMatchPlainAndRelativeGlobsAndTheirKinds) {
    const std::string mainLua = kRoot() + (kRoot().front() == '/' ? "/src/main.lua" : "\\src\\main.lua");
    const std::string config = kGame() + (kRoot().front() == '/' ? "/Assets/Data/a.json" : "\\Assets\\Data\\a.json");
    const FileSystemWatcher lua{.glob = "**/*.lua", .basePath = {}, .kind = 1 | 4};
    EXPECT_TRUE(fileWatcherMatches(lua, mainLua, FileChangeType::Created));
    EXPECT_TRUE(fileWatcherMatches(lua, mainLua, FileChangeType::Deleted));
    EXPECT_FALSE(fileWatcherMatches(lua, mainLua, FileChangeType::Changed)) << "kind 5 has no Change bit";
    EXPECT_FALSE(fileWatcherMatches(lua, config, FileChangeType::Created));
    const FileSystemWatcher name{.glob = "*.lua", .basePath = {}, .kind = 7};
    EXPECT_TRUE(fileWatcherMatches(name, mainLua, FileChangeType::Changed)) << "a name pattern matches in any folder";
    const FileSystemWatcher relative{.glob = "Assets/**/*.json", .basePath = kGame(), .kind = 7};
    EXPECT_TRUE(fileWatcherMatches(relative, config, FileChangeType::Changed));
    EXPECT_FALSE(fileWatcherMatches(relative, mainLua, FileChangeType::Changed));
    EXPECT_FALSE(fileWatcherMatches(relative, kGame(), FileChangeType::Changed)) << "the base itself is not below it";
    // An absolute pattern (LuaLS's own form: "<folder>/**").
    std::string absolute = kGame() + "/**";
    std::ranges::replace(absolute, '\\', '/');
    const FileSystemWatcher tree{.glob = absolute, .basePath = {}, .kind = 7};
    EXPECT_TRUE(fileWatcherMatches(tree, config, FileChangeType::Created));
    EXPECT_FALSE(fileWatcherMatches(tree, mainLua, FileChangeType::Created));
}

} // namespace
