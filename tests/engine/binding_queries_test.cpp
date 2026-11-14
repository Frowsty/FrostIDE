// SPDX-License-Identifier: Apache-2.0
// FEC-13 unit tests for strict captured-response conversion and preview-only LSP dispatch.
#include "frost/engine/binding_queries.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/text/text_buffer.hpp"

#include <gtest/gtest.h>

namespace frost::engine {
namespace {
using rpc::Json;
#ifdef _WIN32
const std::string root = "C:/binding-project";
#else
const std::string root = "/binding-project";
#endif
std::string path(std::string_view name) { return io::joinPath(root, name).value(); }
BindingProviderCapture native() { return {BindingLanguage::Native, "clangd-project", 3, lang::PositionEncoding::Utf8}; }
BindingProviderCapture lua() { return {BindingLanguage::Lua, "luals-project", 9, lang::PositionEncoding::Utf16}; }

BindingDocumentCapture capture(std::string file, std::string contents, std::int64_t version = 4) {
    return {path(file), text::TextBuffer::fromUtf8(std::move(contents)).value().snapshot(), version,
            BindingDocumentRole::UserSource};
}
Json range(std::uint32_t begin, std::uint32_t end) { return lang::toJson(lang::LspRange{{0, begin}, {0, end}}); }
Json locations(const BindingDocumentCapture& document, std::uint32_t begin, std::uint32_t end) {
    return Json::array().push(
        Json::object().set("uri", lang::pathToUri(document.path).value()).set("range", range(begin, end)));
}
Json rename(const BindingDocumentCapture& document, std::uint32_t begin, std::uint32_t end, std::string text) {
    return Json::object().set("documentChanges", Json::array().push(Json::object()
        .set("textDocument", Json::object().set("uri", lang::pathToUri(document.path).value()).set("version", *document.lspVersion))
        .set("edits", Json::array().push(Json::object().set("range", range(begin, end)).set("newText", std::move(text)))));
}

TEST(BindingQueries, EachProviderEncodingMapsToExactCapturedUnicodeBytes) {
    const std::vector documents{capture("main.cpp", "x\xF0\x9F\x98\x80 Cast()"),
                                capture("main.lua", "x\xF0\x9F\x98\x80 Cast()")};
    auto cpp = normalizeBindingReferences(native(), locations(documents[0], 6, 10), documents);
    auto script = normalizeBindingReferences(lua(), locations(documents[1], 4, 8), documents);
    ASSERT_TRUE(cpp);
    ASSERT_TRUE(script);
    ASSERT_EQ(cpp->size(), 1U);
    ASSERT_EQ(script->size(), 1U);
    EXPECT_EQ(cpp->front().text, "Cast");
    EXPECT_EQ(script->front().text, "Cast");
    EXPECT_EQ(cpp->front().location.bytes, script->front().location.bytes);
    EXPECT_EQ(cpp->front().provider, native());
    EXPECT_EQ(script->front().provider, lua());
}

TEST(BindingQueries, MalformedPartialMissingSnapshotAndClampedRangesRefuseWholeResult) {
    const std::vector documents{capture("main.lua", "x\xF0\x9F\x98\x80 Cast()")};
    auto raw = locations(documents[0], 4, 8);
    raw.push(Json::object());
    EXPECT_FALSE(normalizeBindingReferences(lua(), raw, documents));
    EXPECT_FALSE(normalizeBindingReferences(lua(), locations(documents[0], 4, 99), documents));
    EXPECT_FALSE(normalizeBindingReferences(lua(), locations(documents[0], 2, 3), documents));    // Split surrogate.
    EXPECT_FALSE(normalizeBindingReferences(native(), locations(documents[0], 2, 5), documents)); // Split UTF-8.
    EXPECT_FALSE(normalizeBindingReferences(lua(), locations(documents[0], 8, 4), documents));
    EXPECT_FALSE(normalizeBindingReferences(lua(), locations(capture("uncaptured.lua", "Cast"), 0, 4), documents));
}

TEST(BindingQueries, RenameVersionMismatchAndUnsupportedFileOperationFailClosed) {
    const std::vector documents{capture("main.lua", "Cast()")};
    auto other = documents.front();
    other.lspVersion = 3;
    EXPECT_FALSE(normalizeBindingRename(lua(), rename(other, 0, 4, "Trace"), documents));
    auto operation = Json::object().set(
        "documentChanges", Json::array().push(Json::object()
                                                  .set("kind", "rename")
                                                  .set("oldUri", lang::pathToUri(documents[0].path).value())
                                                  .set("newUri", lang::pathToUri(path("other.lua")).value())));
    EXPECT_FALSE(normalizeBindingRename(lua(), operation, documents));
    auto malformed =
        Json::object().set("changes", Json::object().set(lang::pathToUri(documents[0].path).value(),
                                                         Json::array().push(Json::object().set("range", range(0, 4)))));
    EXPECT_FALSE(normalizeBindingRename(lua(), malformed, documents));
}

TEST(BindingQueries, PreviewCombinesActualProviderEditsWithoutMutatingCapturedText) {
    const std::vector documents{capture("main.cpp", "Cast()"), capture("main.lua", "Cast()")};
    auto cpp = normalizeBindingRename(native(), rename(documents[0], 0, 4, "Trace"), documents);
    auto script = normalizeBindingRename(lua(), rename(documents[1], 0, 4, "Trace"), documents);
    ASSERT_TRUE(cpp);
    ASSERT_TRUE(script);
    cpp->insert(cpp->end(), script->begin(), script->end());
    auto preview = combineBindingPreview(*cpp, documents);
    ASSERT_TRUE(preview);
    ASSERT_EQ(preview->size(), 2U);
    for (const auto& file : *preview) {
        EXPECT_EQ(file.snapshot.text(), "Cast()");
        EXPECT_EQ(file.preview, "Trace()");
        ASSERT_EQ(file.edits.size(), 1U);
        EXPECT_EQ(file.edits.front().before, "Cast");
    }
    EXPECT_EQ(documents[0].snapshot.text(), "Cast()");
    EXPECT_EQ(documents[1].snapshot.text(), "Cast()");
}

TEST(BindingQueries, CrossProviderConflictsAndSamePositionDifferentInsertionsAreRejected) {
    const std::vector documents{capture("main.lua", "Cast()")};
    auto edits = normalizeBindingRename(lua(), rename(documents[0], 0, 4, "Trace"), documents).value();
    auto second = edits.front();
    second.provider = native();
    second.after = "Probe";
    edits.push_back(second);
    EXPECT_FALSE(combineBindingPreview(edits, documents));
    edits[1].after = "Trace";
    auto identical = combineBindingPreview(edits, documents);
    ASSERT_TRUE(identical);
    ASSERT_EQ(identical->front().edits.size(), 1U);
    EXPECT_EQ(identical->front().edits.front().corroboratingProviders, std::vector{native()});
    edits = normalizeBindingRename(lua(), rename(documents[0], 0, 0, "A"), documents).value();
    second = edits.front();
    second.after = "B";
    edits.push_back(second);
    EXPECT_FALSE(combineBindingPreview(edits, documents));
}

TEST(BindingQueries, UnrelatedSameVersionSnapshotCannotReplaceTheCapturedDocument) {
    const std::vector documents{capture("main.lua", "Cast() original")};
    const auto edits = normalizeBindingRename(lua(), rename(documents[0], 0, 4, "Trace"), documents).value();
    const std::vector replacement{capture("main.lua", "Cast() changed")};
    EXPECT_EQ(documents[0].snapshot.version(), replacement[0].snapshot.version());
    EXPECT_FALSE(combineBindingPreview(edits, replacement));
}

class ResponseServer final : public lang::ILspRequester {
public:
    lang::PositionEncoding positionEncoding = lang::PositionEncoding::Utf16;
    Json response;
    Json symbolInfoResponse;
    std::function<void()> onReply;
    std::vector<std::string> methods;
    std::vector<Json> parameters;
    sched::Task<Result<Json>> request(std::string method, Json params, std::stop_token) override {
        methods.push_back(std::move(method));
        parameters.push_back(std::move(params));
        if (onReply)
            onReply();
        co_return methods.back() == "textDocument/symbolInfo" ? symbolInfoResponse : response;
    }
    lang::PositionEncoding encoding() const override { return positionEncoding; }
    std::optional<lang::ServerCapabilities> serverCapabilities() const override { return std::nullopt; }
};

BindingProviderRequest request(ResponseServer& server, const BindingDocumentCapture& document,
                               BindingProviderCapture provider) {
    const bool nativeLanguage = provider.language == BindingLanguage::Native;
    return {{std::move(provider),
             {document.path, {ByteOffset{0}, ByteOffset{4}}},
             nativeLanguage ? "" : "Physics.Cast",
             nativeLanguage ? "native-usr" : "",
             document.snapshot},
            &server};
}

TEST(BindingQueries, RevocationBeforeDispatchSendsZeroRequestsAndRestartAfterReplyDropsResult) {
    const std::vector documents{capture("main.lua", "Cast()")};
    ResponseServer server;
    server.response = locations(documents[0], 0, 4);
    auto result = sched::syncWait(
        queryBindingUsages({request(server, documents[0], lua())}, documents, [](const auto&) { return false; }));
    EXPECT_FALSE(result);
    EXPECT_TRUE(server.methods.empty());
    auto untracked = request(server, documents[0], lua());
    untracked.seed.anchorSnapshot.reset();
    result = sched::syncWait(queryBindingUsages({untracked}, documents, [](const auto&) { return true; }));
    EXPECT_FALSE(result);
    EXPECT_TRUE(server.methods.empty());
    bool current = true;
    server.onReply = [&] { current = false; };
    result = sched::syncWait(
        queryBindingUsages({request(server, documents[0], lua())}, documents, [&](const auto&) { return current; }));
    EXPECT_FALSE(result);
    ASSERT_EQ(server.methods.size(), 1U);
    EXPECT_EQ(server.methods.front(), "textDocument/references");
}

TEST(BindingQueries, ExplicitRenameTargetQueriesOnlyThatIdentityAndNeverExpandsAliases) {
    const std::vector documents{capture("main.lua", "Cast()")};
    ResponseServer server;
    server.response = rename(documents[0], 0, 4, "Trace");
    auto preview = sched::syncWait(queryBindingRenamePreview({{request(server, documents[0], lua()), "Trace"}},
                                                             documents, [](const auto&) { return true; }));
    ASSERT_TRUE(preview);
    ASSERT_EQ(server.methods, std::vector<std::string>{"textDocument/rename"});
    EXPECT_EQ(server.parameters.front().stringAt("newName"), "Trace");
    EXPECT_EQ(preview->front().preview, "Trace()");
    EXPECT_EQ(documents.front().snapshot.text(), "Cast()");
}

TEST(BindingQueries, SecondProviderRefusalNeverPublishesFirstProvidersPartialResult) {
    const std::vector documents{capture("main.cpp", "Cast()"), capture("main.lua", "Cast()")};
    ResponseServer cpp, script;
    cpp.positionEncoding = lang::PositionEncoding::Utf8;
    cpp.response = locations(documents[0], 0, 4);
    cpp.symbolInfoResponse = Json::array().push(Json::object().set("usr", "native-usr"));
    script.response = locations(documents[1], 0, 4);
    bool current = true;
    cpp.onReply = [&] {
        if (cpp.methods.back() == "textDocument/references")
            current = false;
    };
    auto result = sched::syncWait(queryBindingUsages(
        {request(cpp, documents[0], native()), request(script, documents[1], lua())}, documents,
        [&](const auto& provider) { return provider.language == BindingLanguage::Native || current; }));
    EXPECT_FALSE(result);
    EXPECT_EQ(cpp.methods.size(), 2U);
    EXPECT_TRUE(script.methods.empty());
}

TEST(BindingQueries, ContainingFunctionOrAmbiguousSymbolInfoCannotAuthorizeNativeQueries) {
    const std::vector documents{capture("main.cpp", "Cast()")};
    ResponseServer server;
    server.positionEncoding = lang::PositionEncoding::Utf8;
    server.symbolInfoResponse = Json::array().push(Json::object().set("usr", "containing-function-not-lambda"));
    server.response = locations(documents[0], 0, 4);
    auto usages = sched::syncWait(
        queryBindingUsages({request(server, documents[0], native())}, documents, [](const auto&) { return true; }));
    EXPECT_FALSE(usages);
    EXPECT_EQ(server.methods, std::vector<std::string>{"textDocument/symbolInfo"});
    server.methods.clear();
    server.symbolInfoResponse =
        Json::array().push(Json::object().set("usr", "native-usr")).push(Json::object().set("usr", "another-overload"));
    auto preview = sched::syncWait(queryBindingRenamePreview({{request(server, documents[0], native()), "Trace"}},
                                                             documents, [](const auto&) { return true; }));
    EXPECT_FALSE(preview);
    EXPECT_EQ(server.methods, std::vector<std::string>{"textDocument/symbolInfo"});
}

TEST(BindingQueries, ExactNativeUsrConfirmationPrecedesActualReferencesRequest) {
    const std::vector documents{capture("main.cpp", "Cast()")};
    ResponseServer server;
    server.positionEncoding = lang::PositionEncoding::Utf8;
    server.symbolInfoResponse = Json::array().push(Json::object().set("usr", "native-usr"));
    server.response = locations(documents[0], 0, 4);
    auto usages = sched::syncWait(
        queryBindingUsages({request(server, documents[0], native())}, documents, [](const auto&) { return true; }));
    ASSERT_TRUE(usages);
    ASSERT_EQ(usages->size(), 1U);
    EXPECT_EQ(usages->front().text, "Cast");
    EXPECT_EQ(server.methods, (std::vector<std::string>{"textDocument/symbolInfo", "textDocument/references"}));
}
TEST(BindingQueries, PhysicalAstSeedUsesExactLexicalProviderDocumentAndDirtySnapshot) {
    auto document = capture("alias/main.cpp", "Cast() dirty", 41);
    document.physicalPath = path("physical/main.cpp");
    ResponseServer server;
    server.positionEncoding = lang::PositionEncoding::Utf8;
    server.symbolInfoResponse = Json::array().push(Json::object().set("usr", "native-usr"));
    server.response = locations(document, 0, 4);
    auto query = request(server, document, native());
    query.seed.anchor.path = document.physicalPath;
    auto actual = sched::syncWait(queryBindingUsages({query}, {document}, [](const auto&) { return true; }));
    ASSERT_TRUE(actual) << actual.error().message();
    ASSERT_EQ(actual->size(), 1U);
    EXPECT_EQ(actual->front().location.path, document.path);
    EXPECT_TRUE(actual->front().capturedSnapshot.sharesStorageWith(document.snapshot));
    EXPECT_EQ(document.lspVersion, 41);
    ASSERT_EQ(server.parameters.size(), 2U);
    for (const auto& params : server.parameters)
        EXPECT_EQ(params.find("textDocument")->stringAt("uri"), lang::pathToUri(document.path).value());
    auto diskResponse = document;
    diskResponse.path = document.physicalPath;
    server.response = locations(diskResponse, 0, 4);
    EXPECT_FALSE(sched::syncWait(queryBindingUsages({query}, {document}, [](const auto&) { return true; })));
}

TEST(BindingQueries, ConflictingPhysicalAliasesAndCrossIdentityCollisionsRefuseBeforeProviderDispatch) {
    auto first = capture("alias/main.cpp", "Cast()");
    first.physicalPath = path("physical/main.cpp");
    auto second = capture("other/main.cpp", "Cast() changed");
    second.physicalPath = first.physicalPath;
    ResponseServer server;
    server.positionEncoding = lang::PositionEncoding::Utf8;
    auto query = request(server, first, native());
    EXPECT_FALSE(sched::syncWait(queryBindingUsages({query}, {first, second}, [](const auto&) { return true; })));
    EXPECT_TRUE(server.methods.empty());
    second.physicalPath = path("other/actual.cpp");
    second.path = first.physicalPath;
    EXPECT_FALSE(sched::syncWait(queryBindingUsages({query}, {first, second}, [](const auto&) { return true; })));
    EXPECT_TRUE(server.methods.empty());
}

} // namespace
} // namespace frost::engine
