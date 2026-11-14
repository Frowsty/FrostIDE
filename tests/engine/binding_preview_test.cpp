// SPDX-License-Identifier: Apache-2.0
// Selected-surface planner unit fixtures, not claims about actual LuaLS or compiler acceptance.
#include "frost/engine/binding_queries.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/text/text_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace frost::engine {
namespace {
using rpc::Json;
#ifdef _WIN32
const std::string root = "C:/selected-binding";
#else
const std::string root = "/selected-binding";
#endif
std::string path(std::string_view file) { return io::joinPath(root, file).value(); }
text::Snapshot snapshot(std::string contents) {
    return text::TextBuffer::fromUtf8(std::move(contents)).value().snapshot();
}

SchemaLuaEntry entry(std::string name, std::string alias, std::uint32_t line) {
    SchemaLuaEntry result;
    result.kind = "GlobalFunction";
    result.name = name;
    result.fullName = name;
    result.qualifiedName = std::move(name);
    result.sameAs = std::move(alias);
    result.source = SchemaSource{"Bindings.cpp", line};
    result.signature = result.name + "()";
    return result;
}

class RenameServer final : public lang::ILspRequester {
public:
    lang::PositionEncoding positionEncoding = lang::PositionEncoding::Utf16;
    Json edits;
    std::vector<std::string> methods;
    sched::Task<Result<Json>> request(std::string method, Json, std::stop_token) override {
        methods.push_back(method);
        if (method == "textDocument/symbolInfo")
            co_return Json::array().push(Json::object().set("usr", "native-usr"));
        co_return edits;
    }
    lang::PositionEncoding encoding() const override { return positionEncoding; }
    std::optional<lang::ServerCapabilities> serverCapabilities() const override { return std::nullopt; }
};

Json fileEdit(const BindingDocumentCapture& document, text::ByteRange bytes, lang::PositionEncoding encoding,
              std::string replacement) {
    const lang::LspRange range{lang::toLspPosition(document.snapshot, bytes.begin, encoding),
                               lang::toLspPosition(document.snapshot, bytes.end, encoding)};
    return Json::object()
        .set("textDocument",
             Json::object().set("uri", lang::pathToUri(document.path).value()).set("version", *document.lspVersion))
        .set("edits", Json::array().push(
                          Json::object().set("range", lang::toJson(range)).set("newText", std::move(replacement))));
}

struct Rig {
    SchemaDocument schema;
    std::shared_ptr<BindingIndex> index;
    std::vector<BindingDocumentCapture> documents;
    GeneratedEngineLuaMetadata metadata;
    RenameServer lua, native;
    SelectedLuaBindingRename selected;

    explicit Rig(bool literalBelongsToSelectedAlias = true) {
        schema.luaEntries = {entry("Base", {}, 2), entry("Cast", "Base", 1)};
        auto mapping =
            NativeBindingMapping{literalBelongsToSelectedAlias ? "Cast" : "Base",
                                 *schema.luaEntries[literalBelongsToSelectedAlias ? 1 : 0].source,
                                 "native-usr",
                                 "Native",
                                 {path("Native.hpp"), {ByteOffset{0}, ByteOffset{6}}},
                                 {},
                                 {path("Bindings.cpp"), {ByteOffset{0}, ByteOffset{45}}},
                                 "callable",
                                 "function",
                                 {path("Bindings.cpp"), {ByteOffset{13}, ByteOffset{20}}},
                                 BindingNameLiteral{{path("Bindings.cpp"), {ByteOffset{5}, ByteOffset{11}}},
                                                    literalBelongsToSelectedAlias ? "Cast" : "Base",
                                                    literalBelongsToSelectedAlias ? "\"Cast\"" : "\"Base\""},
                                 {},
                                 BindingSignatureLiteral{{{path("Bindings.cpp"), {ByteOffset{34}, ByteOffset{42}}},
                                                          literalBelongsToSelectedAlias ? "Cast()" : "Base()",
                                                          literalBelongsToSelectedAlias ? "\"Cast()\"" : "\"Base()\""},
                                                         0,
                                                         4},
                                 {}};
        index = std::make_shared<BindingIndex>(BindingIndex::create(root, 13, schema, std::vector{mapping}).value());
        metadata = generateEngineLuaMetadata(schema);
        documents = {
            {path("Bindings.cpp"), snapshot("bind(\"Cast\", &Native, {.Signature=\"Cast()\"});"), 7,
             BindingDocumentRole::UserSource},
            {path("frost_schema.lua"), snapshot(metadata.text), 17, BindingDocumentRole::GeneratedEngineMetadata},
            {path("main.lua"), snapshot("Cast()\nBase()"), 19, BindingDocumentRole::UserSource},
            {path("Native.hpp"), snapshot("Native()"), 23, BindingDocumentRole::UserSource}};
        auto anchors = captureBindingLuaAnchors(metadata, documents[1].path, documents[1].snapshot).value();
        const auto anchor = std::ranges::find(anchors, std::string("Cast"), &LuaBindingAnchor::luaQualifiedName);
        selected = {{{{BindingLanguage::Lua, "luals", 11, lang::PositionEncoding::Utf16},
                      anchor->declaration,
                      "Cast",
                      {},
                      anchor->capturedSnapshot},
                     &lua},
                    "Trace"};
        lua.edits = Json::object().set(
            "documentChanges",
            Json::array()
                .push(fileEdit(documents[1], anchor->declaration.bytes, lang::PositionEncoding::Utf16, "Trace"))
                .push(fileEdit(documents[2], {ByteOffset{0}, ByteOffset{4}}, lang::PositionEncoding::Utf16, "Trace")));
        native.positionEncoding = lang::PositionEncoding::Utf8;
        native.edits =
            Json::object().set("documentChanges", Json::array()
                                                      .push(fileEdit(documents[3], {ByteOffset{0}, ByteOffset{6}},
                                                                     lang::PositionEncoding::Utf8, "RenamedNative"))
                                                      .push(fileEdit(documents[0], {ByteOffset{14}, ByteOffset{20}},
                                                                     lang::PositionEncoding::Utf8, "RenamedNative")));
    }
    Result<BindingSelectedRenamePreview> run(std::vector<BindingRenameRequest> nativeChoices = {}) {
        return sched::syncWait(querySelectedBindingRenamePreview(index, selected, std::move(nativeChoices), documents,
                                                                 [](const auto&) { return true; }));
    }
};

TEST(BindingSelectedPreview, SelectedAliasLiteralAndActualLuaEditsHaveSeparateRegenerationIntent) {
    Rig rig;
    auto preview = rig.run();
    ASSERT_TRUE(preview) << (preview ? "" : preview.error().message());
    EXPECT_EQ(preview->previousLuaIdentity, "Cast");
    EXPECT_EQ(preview->renamedLuaIdentity, "Trace");
    EXPECT_EQ(preview->sourceFiles.size(), 2U);
    for (const auto& file : preview->sourceFiles) {
        if (file.path == rig.documents[0].path) {
            EXPECT_EQ(file.preview, "bind(\"Trace\", &Native, {.Signature=\"Trace()\"});");
            ASSERT_EQ(file.edits.size(), 2U);
            EXPECT_EQ(file.edits.front().origin, BindingPreviewOrigin::RegistrationLiteral);
        } else {
            EXPECT_EQ(file.preview, "Trace()\nBase()");
        }
    }
    EXPECT_EQ(preview->generatedResource.path, rig.documents[1].path);
    EXPECT_EQ(preview->generatedResource.before.text(), rig.metadata.text);
    auto expectedSchema = rig.schema;
    expectedSchema.luaEntries[1].name = "Trace";
    expectedSchema.luaEntries[1].fullName = "Trace";
    expectedSchema.luaEntries[1].qualifiedName = "Trace";
    expectedSchema.luaEntries[1].signature = "Trace()";
    EXPECT_EQ(preview->generatedResource.regenerated.text, generateEngineLuaStubs(expectedSchema));
    EXPECT_EQ(rig.documents[0].snapshot.text(), "bind(\"Cast\", &Native, {.Signature=\"Cast()\"});");
    EXPECT_EQ(rig.documents[1].snapshot.text(), rig.metadata.text);
}

TEST(BindingSelectedPreview, InheritedOtherAliasLiteralNeverAuthorizesSelectedAliasRewrite) {
    Rig rig(false);
    EXPECT_FALSE(rig.run());
    EXPECT_TRUE(rig.lua.methods.empty());
}

TEST(BindingSelectedPreview, ChangedLiteralOrUnclassifiedSourceRefusesBeforeLanguageServerRequest) {
    Rig rig;
    rig.documents[0].snapshot = snapshot("bind(\"Other\", &Native);");
    EXPECT_FALSE(rig.run());
    EXPECT_TRUE(rig.lua.methods.empty());
    Rig external;
    external.documents[0].role = BindingDocumentRole::ExternalReadOnly;
    EXPECT_FALSE(external.run());
    EXPECT_TRUE(external.lua.methods.empty());
}

TEST(BindingSelectedPreview, ActualMetadataEditMustExactlyMatchRegenerationAndCannotWriteAnotherCache) {
    Rig rig;
    rig.lua.edits = Json::object().set(
        "documentChanges", Json::array().push(fileEdit(rig.documents[1], {ByteOffset{0}, ByteOffset{0}},
                                                       lang::PositionEncoding::Utf16, "arbitrary cache edit")));
    EXPECT_FALSE(rig.run());
    Rig external;
    external.documents.push_back({path("external.lua"), snapshot("Cast()"), 1, BindingDocumentRole::ExternalReadOnly});
    external.lua.edits = Json::object().set(
        "documentChanges", Json::array().push(fileEdit(external.documents.back(), {ByteOffset{0}, ByteOffset{4}},
                                                       lang::PositionEncoding::Utf16, "Trace")));
    EXPECT_FALSE(external.run());
}

TEST(BindingSelectedPreview, GenericPreviewNeverTurnsGeneratedDeclarationEditsIntoOrdinarySourceEdits) {
    Rig rig;
    auto preview = sched::syncWait(
        queryBindingRenamePreview({{rig.selected.target, "Trace"}}, rig.documents, [](const auto&) { return true; }));
    EXPECT_FALSE(preview);
}

TEST(BindingSelectedPreview, ExplicitNativeRenameCombinesVerifiedNativeEditsWithSelectedLuaLiteral) {
    Rig rig;
    BindingProviderRequest native{{{BindingLanguage::Native, "clangd", 5, lang::PositionEncoding::Utf8},
                                   {rig.documents[3].path, {ByteOffset{0}, ByteOffset{6}}},
                                   {},
                                   "native-usr",
                                   rig.documents[3].snapshot},
                                  &rig.native};
    auto preview = rig.run({{native, "RenamedNative"}});
    ASSERT_TRUE(preview) << (preview ? "" : preview.error().message());
    EXPECT_EQ(rig.native.methods, (std::vector<std::string>{"textDocument/symbolInfo", "textDocument/rename"}));
    const auto source = std::ranges::find(preview->sourceFiles, rig.documents[0].path, &BindingPreviewFile::path);
    ASSERT_NE(source, preview->sourceFiles.end());
    EXPECT_EQ(source->preview, "bind(\"Trace\", &RenamedNative, {.Signature=\"Trace()\"});");
}

TEST(BindingSelectedPreview, SelectedNativeRenameNullOrEmptyResponseCannotPublishPartialLuaPreview) {
    Rig rig;
    BindingProviderRequest native{{{BindingLanguage::Native, "clangd", 5, lang::PositionEncoding::Utf8},
                                   {rig.documents[3].path, {ByteOffset{0}, ByteOffset{6}}},
                                   {},
                                   "native-usr",
                                   rig.documents[3].snapshot},
                                  &rig.native};
    rig.native.edits = Json();
    EXPECT_FALSE(rig.run({{native, "RenamedNative"}}));
    EXPECT_TRUE(rig.lua.methods.empty());
    EXPECT_EQ(rig.native.methods, (std::vector<std::string>{"textDocument/symbolInfo", "textDocument/rename"}));
    rig.native.edits = Json::object().set("changes", Json::object());
    EXPECT_FALSE(rig.run({{native, "RenamedNative"}}));
    EXPECT_TRUE(rig.lua.methods.empty());
}

TEST(BindingSelectedPreview, CallableWithoutAstSignatureEvidenceRefusesBeforeAnyRenameRequest) {
    Rig rig;
    auto mappings = rig.index->findLua("Cast")->native;
    for (auto& mapping : mappings)
        mapping.docSignatureLiteral.reset();
    rig.index = std::make_shared<BindingIndex>(BindingIndex::create(root, 14, rig.schema, mappings).value());
    EXPECT_FALSE(rig.run());
    EXPECT_TRUE(rig.lua.methods.empty());
}

TEST(BindingSelectedPreview, PrimaryRenameRegeneratesDerivedAliasRelationWithoutClaimingLuaLsEditedComment) {
    Rig rig(false);
    rig.documents[0].snapshot = snapshot("bind(\"Base\", &Native, {.Signature=\"Base()\"});");
    const auto anchors =
        captureBindingLuaAnchors(rig.metadata, rig.documents[1].path, rig.documents[1].snapshot).value();
    const auto primary = std::ranges::find(anchors, std::string("Base"), &LuaBindingAnchor::luaQualifiedName);
    rig.selected.target.seed.luaQualifiedName = "Base";
    rig.selected.target.seed.anchor = primary->declaration;
    rig.selected.target.seed.anchorSnapshot = primary->capturedSnapshot;
    rig.lua.edits = Json::object().set(
        "documentChanges",
        Json::array()
            .push(fileEdit(rig.documents[1], primary->declaration.bytes, lang::PositionEncoding::Utf16, "Trace"))
            .push(fileEdit(rig.documents[2], {ByteOffset{7}, ByteOffset{11}}, lang::PositionEncoding::Utf16, "Trace")));
    auto preview = rig.run();
    ASSERT_TRUE(preview) << (preview ? "" : preview.error().message());
    EXPECT_NE(preview->generatedResource.regenerated.text.find("--- Same as: Trace"), std::string::npos);
    EXPECT_NE(preview->generatedResource.observedLanguageServerText.find("--- Same as: Base"), std::string::npos);
    EXPECT_NE(preview->generatedResource.regenerated.text.find("function Cast("), std::string::npos);
    ASSERT_EQ(preview->generatedResource.observedServerEdits.size(), 1U);
    EXPECT_EQ(preview->generatedResource.observedServerEdits.front().before, "Base");
    EXPECT_EQ(preview->generatedResource.observedServerEdits.front().after, "Trace");
    EXPECT_FALSE(preview->applicationSafe);
    EXPECT_FALSE(preview->embeddedLanguageReferencesVerified);
}
TEST(BindingSelectedPreview, PhysicalAstLiteralJoinsLexicalAliasWithoutChangingIndexProvenance) {
    Rig rig;
    const auto physical = rig.documents[0].path;
    rig.documents[0].physicalPath = physical;
    rig.documents[0].path = path("alias/Bindings.cpp");
    const auto before = rig.documents[0].snapshot;
    auto preview = rig.run();
    ASSERT_TRUE(preview) << preview.error().message();
    const auto at = std::ranges::find(preview->sourceFiles, rig.documents[0].path, &BindingPreviewFile::path);
    ASSERT_NE(at, preview->sourceFiles.end());
    EXPECT_EQ(at->preview, "bind(\"Trace\", &Native, {.Signature=\"Trace()\"});");
    EXPECT_TRUE(at->snapshot.sharesStorageWith(before));
    ASSERT_EQ(at->edits.size(), 2U);
    for (const auto& edit : at->edits)
        EXPECT_EQ(edit.location.path, rig.documents[0].path);
    EXPECT_EQ(rig.index->findLua("Cast")->native.front().nameLiteral->range.path, physical);
    EXPECT_EQ(std::ranges::count(preview->sourceFiles, physical, &BindingPreviewFile::path), 0);
}

TEST(BindingSelectedPreview, EmbeddedLuaProviderEditsMapThroughWholeEscapedAstLiteral) {
    Rig rig;
    const std::string spelling = "\"Cast()\\nlocal Cast = function() end; Cast()\"";
    const auto nativePath = path("Calls.cpp");
    const auto projectedPath = path(".binding-projection.lua");
    const std::string decoded = "Cast()\nlocal Cast = function() end; Cast()";
    rig.documents.push_back({nativePath, snapshot("execute(" + spelling + ");"), {}, BindingDocumentRole::UserSource});
    BindingDocumentCapture projection{projectedPath, snapshot(decoded), 29, BindingDocumentRole::UserSource};
    rig.documents.push_back(projection);
    BindingEmbeddedLuaCapture chunk{{{nativePath, {ByteOffset{8}, ByteOffset{8 + spelling.size()}}}, decoded, spelling},
                                    projection};
    auto changes = *rig.lua.edits.find("documentChanges");
    changes.push(fileEdit(projection, {ByteOffset{0}, ByteOffset{4}}, lang::PositionEncoding::Utf16, "Trace"));
    rig.lua.edits.set("documentChanges", std::move(changes));
    auto result = sched::syncWait(querySelectedBindingRenamePreview(rig.index, rig.selected, {}, rig.documents,
                                                                    [](const auto&) { return true; }, {}, {chunk}));
    ASSERT_TRUE(result) << result.error().message();
    const auto file = std::ranges::find(result->sourceFiles, nativePath, &BindingPreviewFile::path);
    ASSERT_NE(file, result->sourceFiles.end());
    EXPECT_EQ(file->preview, "execute(\"Trace()\\nlocal Cast = function() end; Cast()\");");
    EXPECT_EQ(file->edits.front().origin, BindingPreviewOrigin::EmbeddedLuaLiteral);
    EXPECT_EQ(file->edits.front().before, spelling);
    EXPECT_EQ(std::ranges::count(result->sourceFiles, projectedPath, &BindingPreviewFile::path), 0);
    // Semantic response chooses only the global call. Projection does not rename same-name locals.
    EXPECT_FALSE(result->applicationSafe); // The app must still supply full captured-profile admission.
}
TEST(BindingSelectedPreview, EmbeddedLuaAstSpellingMismatchRefusesWholePreview) {
    Rig rig;
    BindingDocumentCapture projection{path(".binding-projection.lua"), snapshot("Cast()"), 29,
                                      BindingDocumentRole::UserSource};
    rig.documents.push_back({path("Calls.cpp"), snapshot("\"Other()\""), {}, BindingDocumentRole::UserSource});
    rig.documents.push_back(projection);
    BindingEmbeddedLuaCapture chunk{{{path("Calls.cpp"), {ByteOffset{0}, ByteOffset{8}}}, "Cast()", "\"Cast()\""},
                                    projection};
    auto changes = *rig.lua.edits.find("documentChanges");
    changes.push(fileEdit(projection, {ByteOffset{0}, ByteOffset{4}}, lang::PositionEncoding::Utf16, "Trace"));
    rig.lua.edits.set("documentChanges", std::move(changes));
    auto result = sched::syncWait(querySelectedBindingRenamePreview(rig.index, rig.selected, {}, rig.documents,
                                                                    [](const auto&) { return true; }, {}, {chunk}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
}

} // namespace
} // namespace frost::engine
