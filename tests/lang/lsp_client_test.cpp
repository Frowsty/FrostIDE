// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for LspClient (lsp_client.hpp) against a scripted in-memory server: the initialize
// handshake and capability negotiation (general.positionEncodings only, a legacy offsetEncoding
// answer, the UTF-16 default), Busy before initialize, the server requests a client answers, the document
// notifications with their versions, publishDiagnostics routing, and shutdown/exit with and
// without an answer.

#include "frost/lang/lsp_client.hpp"
#include "frost/lang/uri.hpp"

#include "lang/fake_server.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::lang::ContentChange;
using frost::lang::InitializeOptions;
using frost::lang::Json;
using frost::lang::LspClient;
using frost::lang::PositionEncoding;
using frost::test::eventually;
using frost::test::FakeServer;
using frost::test::LangEnv;
using frost::test::MemoryPipe;

#ifdef _WIN32
const std::string& kRoot() {
    static const std::string value = "C:\\work\\proj";
    return value;
}
#else
const std::string& kRoot() {
    static const std::string value = "/work/proj";
    return value;
}
#endif

InitializeOptions options() {
    InitializeOptions init;
    init.rootPath = kRoot();
    init.processId = 1234;
    init.clientVersion = "test";
    init.configuration = [](std::string_view section) { return section == "frost" ? Json("yes") : Json(); };
    return init;
}

// A server that answers initialize with `result` and shutdown with null.
FakeServer::Script answering(Json result) {
    return [result = std::move(result)](FakeServer& server, const Json& message) {
        const std::string_view method = message.stringAt("method");
        if (method == "initialize" || method == "shutdown") {
            server.respond(*message.find("id"), method == "initialize" ? result : Json());
        }
        return true;
    };
}

Json capabilitiesWith(Json capabilities) {
    return Json::object()
        .set("capabilities", std::move(capabilities))
        .set("serverInfo", Json::object().set("name", "fake").set("version", "1"));
}

TEST(LspClientTest, InitializeNegotiatesUtf8AndSendsInitialized) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(
                                Json::object()
                                    .set("positionEncoding", "utf-8")
                                    .set("textDocumentSync", Json::object()
                                                                 .set("openClose", true)
                                                                 .set("change", 2)
                                                                 .set("save", Json::object().set("includeText", false)))
                                    .set("completionProvider", Json::object())
                                    .set("definitionProvider", true))));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    EXPECT_FALSE(client.ready());
    auto capabilities = env.run(client.initialize());
    ASSERT_TRUE(capabilities.has_value()) << capabilities.error().describe();
    EXPECT_TRUE(client.ready());
    EXPECT_EQ(capabilities->positionEncoding, PositionEncoding::Utf8);
    EXPECT_EQ(client.positionEncoding(), PositionEncoding::Utf8);
    EXPECT_EQ(capabilities->sync, frost::lang::TextDocumentSyncKind::Incremental);
    EXPECT_TRUE(capabilities->save);
    EXPECT_FALSE(capabilities->saveIncludeText);
    EXPECT_TRUE(capabilities->completion);
    EXPECT_TRUE(capabilities->definition);
    EXPECT_FALSE(capabilities->hover);
    EXPECT_EQ(capabilities->serverName, "fake");

    ASSERT_TRUE(server.waitForMessages(2));
    const auto received = server.received();
    EXPECT_EQ(received[0].stringAt("method"), "initialize");
    const Json* params = received[0].find("params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->integerAt("processId"), 1234);
    EXPECT_EQ(params->stringAt("rootUri"), frost::lang::pathToUri(kRoot()).value());
    EXPECT_EQ(params->find("clientInfo")->stringAt("name"), "FrostIDE");
    const Json* encodings = params->find("capabilities")->find("general")->find("positionEncodings");
    ASSERT_NE(encodings, nullptr);
    EXPECT_EQ(*encodings, Json::array({Json("utf-8"), Json("utf-16")}));
    // LSP 3.17 negotiation only: clangd's deprecated offsetEncoding extension is not offered.
    EXPECT_EQ(params->find("capabilities")->find("offsetEncoding"), nullptr);
    EXPECT_EQ(received[1].stringAt("method"), "initialized");
    EXPECT_EQ(received[1].find("id"), nullptr);
}

TEST(LspClientTest, Utf16OnlyIsTheWholeStandardOfferAndNoLegacyOffer) {
    auto init = options();
    init.utf16Only = true;
    const auto params = frost::lang::initializeParams(init);
    ASSERT_TRUE(params.has_value());
    const auto* capabilities = params->find("capabilities");
    ASSERT_NE(capabilities, nullptr);
    const auto* general = capabilities->find("general");
    ASSERT_NE(general, nullptr);
    const auto* standard = general->find("positionEncodings");
    ASSERT_NE(standard, nullptr);
    EXPECT_EQ(*standard, Json::array({Json("utf-16")}));
    EXPECT_EQ(capabilities->find("offsetEncoding"), nullptr);
}

// A server that omits positionEncoding uses UTF-16 (LSP 3.17), even though UTF-8 was preferred.
TEST(LspClientTest, OmittedPositionEncodingNegotiatesUtf16) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object())));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    const auto result = env.run(client.initialize());
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->positionEncoding, PositionEncoding::Utf16);
    EXPECT_EQ(client.encoding(), PositionEncoding::Utf16);
}

// The server's positionEncoding choice from the offer is the client's encoding.
TEST(LspClientTest, SelectedUtf8PositionEncodingIsUsed) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object().set("positionEncoding", "utf-8"))));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    const auto result = env.run(client.initialize());
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(client.encoding(), PositionEncoding::Utf8);
}

// "utf-32" is a valid LSP kind but was not offered, so the handshake fails.
TEST(LspClientTest, RejectsUnofferedUtf32) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object().set("positionEncoding", "utf-32"))));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    const auto result = env.run(client.initialize());
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(client.ready());
}

TEST(LspClientTest, RejectsServerEncodingOutsideTheOffer) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object().set("positionEncoding", "utf-8"))));
    auto init = options();
    init.utf16Only = true;
    LspClient client(pipe.clientStream(), env.pool(), init, {});
    const auto result = env.run(client.initialize());
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(client.ready());
}

TEST(LspClientTest, RejectsConflictingStandardAndLegacyEncodings) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    auto reply = capabilitiesWith(Json::object().set("positionEncoding", "utf-16"));
    reply.set("offsetEncoding", "utf-8");
    FakeServer server(pipe, answering(std::move(reply)));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    const auto result = env.run(client.initialize());
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(client.ready());
}

TEST(LspClientTest, OffsetEncodingIsTheFallbackAndUtf16TheDefault) {
    const auto parse = [](const char* text) {
        return frost::lang::parseInitializeResult(frost::lang::parseJson(text).value()).positionEncoding;
    };
    EXPECT_EQ(parse(R"({"capabilities":{},"offsetEncoding":"utf-8"})"), PositionEncoding::Utf8);
    EXPECT_EQ(parse(R"({"capabilities":{"offsetEncoding":"utf-8"}})"), PositionEncoding::Utf8);
    EXPECT_EQ(parse(R"({"capabilities":{"positionEncoding":"utf-16"},"offsetEncoding":"utf-8"})"),
              PositionEncoding::Utf16);
    EXPECT_EQ(parse(R"({"capabilities":{"positionEncoding":"utf-32"}})"), PositionEncoding::Utf16);
    EXPECT_EQ(parse(R"({"capabilities":{}})"), PositionEncoding::Utf16);
    const auto sync = frost::lang::parseInitializeResult(
        frost::lang::parseJson(R"({"capabilities":{"textDocumentSync":1}})").value());
    EXPECT_EQ(sync.sync, frost::lang::TextDocumentSyncKind::Full);
    EXPECT_TRUE(sync.openClose);
}

TEST(LspClientTest, FrostPlatformIsOnlyAcceptedFromItsVersionedRealCapability) {
    const auto parse = [](const char* text) {
        return frost::lang::parseInitializeResult(frost::lang::parseJson(text).value());
    };
    EXPECT_FALSE(parse(R"({"capabilities":{}})").frostPlatform);
    EXPECT_FALSE(parse(R"({"capabilities":{"frostClangFeatureModule":{"engine":"other","schema":1}}})").frostPlatform);
    EXPECT_FALSE(
        parse(R"({"capabilities":{"frostClangFeatureModule":{"engine":"frost-clang","schema":2}}})").frostPlatform);
    const auto actual = parse(
        R"({"capabilities":{"frostClangFeatureModule":{"engine":"frost-clang","schema":1,"llvmVersion":"23.1.4"},"definitionProvider":true}})");
    EXPECT_TRUE(actual.frostPlatform);
    EXPECT_EQ(actual.frostLlvmVersion, "23.1.4");
    EXPECT_TRUE(actual.definition);
}
TEST(LspClientTest, NotificationsWaitForInitialize) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, nullptr);
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    auto sent = client.didOpen("file:///x.cpp", "cpp", 1, "int x;");
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), ErrorCode::Busy);
    auto requested = env.run(client.request("textDocument/hover", Json()));
    ASSERT_FALSE(requested.has_value());
    EXPECT_EQ(requested.error().code(), ErrorCode::Busy);
}

TEST(LspClientTest, DocumentNotificationsCarryVersionsAndRanges) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object().set("positionEncoding", "utf-8"))));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    ASSERT_TRUE(env.run(client.initialize()).has_value());
    const std::string uri = "file:///work/a.cpp";
    ASSERT_TRUE(client.didOpen(uri, "cpp", 1, "int a;\n").has_value());
    const std::vector<ContentChange> changes = {
        ContentChange{
            .range = frost::lang::LspRange{.start = {.line = 0, .character = 4}, .end = {.line = 0, .character = 5}},
            .text = "b"},
    };
    ASSERT_TRUE(client.didChange(uri, 2, changes).has_value());
    ASSERT_TRUE(client.didSave(uri).has_value());
    ASSERT_TRUE(client.didClose(uri).has_value());
    ASSERT_TRUE(server.waitForMessages(6));
    const auto received = server.received();
    EXPECT_EQ(received[2].stringAt("method"), "textDocument/didOpen");
    const Json* opened = received[2].find("params")->find("textDocument");
    EXPECT_EQ(opened->stringAt("uri"), uri);
    EXPECT_EQ(opened->stringAt("languageId"), "cpp");
    EXPECT_EQ(opened->integerAt("version"), 1);
    EXPECT_EQ(opened->stringAt("text"), "int a;\n");
    EXPECT_EQ(received[3].stringAt("method"), "textDocument/didChange");
    EXPECT_EQ(received[3].find("params")->find("textDocument")->integerAt("version"), 2);
    EXPECT_EQ(received[3].find("params")->find("contentChanges")->dump(),
              R"([{"range":{"start":{"line":0,"character":4},"end":{"line":0,"character":5}},"text":"b"}])");
    EXPECT_EQ(received[4].stringAt("method"), "textDocument/didSave");
    EXPECT_EQ(received[4].find("params")->find("text"), nullptr); // The server did not ask for it.
    EXPECT_EQ(received[5].stringAt("method"), "textDocument/didClose");
}

TEST(LspClientTest, AnswersServerRequestsAndRoutesNotifications) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        const std::string_view method = message.stringAt("method");
        if (method == "initialize") {
            s.respond(*message.find("id"), capabilitiesWith(Json::object()));
        } else if (method == "initialized") {
            s.requestClient(1, "workspace/configuration",
                            Json::object().set("items", Json::array({Json::object().set("section", "frost"),
                                                                     Json::object().set("section", "other")})));
            s.requestClient(2, "window/workDoneProgress/create", Json::object().set("token", "t"));
            s.requestClient(3, "workspace/applyEdit", Json::object());
            s.notifyClient("window/logMessage", Json::object().set("type", 2).set("message", "careful"));
            s.notifyClient("textDocument/publishDiagnostics",
                           Json::object().set("uri", "file:///x").set("diagnostics", Json::array()));
            s.notifyClient("$/progress", Json::object());
        }
        return true;
    });
    struct Seen {
        std::atomic<int> diagnostics{0};
        std::atomic<int> messages{0};
        std::atomic<int> other{0};
    };
    auto seen = std::make_shared<Seen>();
    LspClient::Handlers handlers;
    handlers.onDiagnostics = [seen](const Json&) { ++seen->diagnostics; };
    handlers.onMessage = [seen](int type, std::string_view text) {
        if (type == 2 && text == "careful") {
            ++seen->messages;
        }
    };
    handlers.onNotification = [seen](std::string_view method, const Json&) {
        if (method == "$/progress") {
            ++seen->other;
        }
    };
    LspClient client(pipe.clientStream(), env.pool(), options(), std::move(handlers));
    ASSERT_TRUE(env.run(client.initialize()).has_value());
    ASSERT_TRUE(server.waitForMessages(5)); // initialize, initialized, three answers.
    const auto received = server.received();
    EXPECT_EQ(received[2].find("result")->dump(), R"(["yes",null])");
    EXPECT_TRUE(received[3].find("result")->isNull());
    EXPECT_EQ(received[4].find("result")->boolAt("applied"), false);
    EXPECT_TRUE(eventually([&] { return seen->diagnostics == 1 && seen->messages == 1 && seen->other == 1; }));
}

TEST(LspClientTest, ShutdownSendsShutdownThenExit) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, answering(capabilitiesWith(Json::object())));
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    ASSERT_TRUE(env.run(client.initialize()).has_value());
    auto down = env.run(client.shutdown(5s));
    ASSERT_TRUE(down.has_value()) << down.error().describe();
    EXPECT_FALSE(client.ready());
    ASSERT_TRUE(server.waitForMessages(4));
    const auto received = server.received();
    EXPECT_EQ(received[2].stringAt("method"), "shutdown");
    EXPECT_EQ(received[3].stringAt("method"), "exit");
    EXPECT_TRUE(eventually([&] { return pipe.clientClosedWrite(); }));
}

TEST(LspClientTest, ShutdownTimesOutWhenTheServerHangs) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        if (message.stringAt("method") == "initialize") {
            s.respond(*message.find("id"), capabilitiesWith(Json::object()));
        }
        return true; // Never answers shutdown.
    });
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    ASSERT_TRUE(env.run(client.initialize()).has_value());
    const auto begin = std::chrono::steady_clock::now();
    auto down = env.run(client.shutdown(200ms));
    ASSERT_FALSE(down.has_value());
    EXPECT_EQ(down.error().code(), ErrorCode::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 5s);
    // The abandoned shutdown is cancelled, then exit follows.
    ASSERT_TRUE(server.waitForMessages(5));
    const auto received = server.received();
    EXPECT_EQ(received[3].stringAt("method"), "$/cancelRequest");
    EXPECT_EQ(received[4].stringAt("method"), "exit");
}

TEST(LspClientTest, InitializeFailsWhenTheServerDies) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json&) {
        s.sendRaw("Content-Length: 50\r\n\r\n{\"jsonrpc\":"); // A torn frame, then EOF.
        s.closeOutput();
        return false;
    });
    LspClient client(pipe.clientStream(), env.pool(), options(), {});
    auto capabilities = env.run(client.initialize());
    ASSERT_FALSE(capabilities.has_value());
    EXPECT_EQ(capabilities.error().code(), ErrorCode::IoError);
    EXPECT_FALSE(client.ready());
}

} // namespace
