// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for semantic tokens and inactive regions (semantic_tokens.hpp, ADR 0047): the legend
// from the server's capabilities, decoding a recorded relative stream into byte ranges, delta
// edits and their errors, the full and delta requests against the scripted server (with the
// fallback to a full request), the inactive-regions notification, and the client capabilities.

#include "frost/lang/lsp_client.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "lang/feature_server.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace fl = frost::lang;
using fl::Json;
using fl::SemanticModifier;
using fl::SemanticTokenType;
using frost::ByteOffset;
using frost::test::FeatureServer;
using frost::test::workPath;

Json names(std::initializer_list<const char*> list) {
    Json out = Json::array();
    for (const char* name : list) {
        out.push(Json(name));
    }
    return out;
}

Json integersJson(const std::vector<std::uint32_t>& values) {
    Json out = Json::array();
    for (const std::uint32_t value : values) {
        out.push(Json(static_cast<std::int64_t>(value)));
    }
    return out;
}

// clangd's legend order for the types and modifiers the tests use, plus one unknown name each.
Json legendJson() {
    return Json::object()
        .set("tokenTypes", names({"variable", "parameter", "property", "macro", "comment", "fancyThing"}))
        .set("tokenModifiers", names({"declaration", "readonly", "static", "functionScope", "madeUp"}));
}

fl::SemanticTokensLegend legend() { return fl::parseSemanticTokensLegend(legendJson()).value(); }

frost::text::Snapshot snapshotOf(std::string text) {
    return frost::text::TextBuffer::fromUtf8(std::move(text)).value().current();
}

frost::text::ByteRange bytes(std::uint64_t begin, std::uint64_t end) {
    return frost::text::ByteRange{ByteOffset{begin}, ByteOffset{end}};
}

TEST(SemanticTokensTest, LegendMapsNamesToTypesAndModifierBits) {
    const fl::SemanticTokensLegend parsed = legend();
    ASSERT_EQ(parsed.types.size(), 6U);
    EXPECT_EQ(parsed.types[0], SemanticTokenType::Variable);
    EXPECT_EQ(parsed.types[3], SemanticTokenType::Macro);
    EXPECT_EQ(parsed.types[5], SemanticTokenType::Unknown);
    ASSERT_EQ(parsed.modifierBits.size(), 5U);
    EXPECT_EQ(parsed.modifierBits[1], static_cast<std::uint32_t>(SemanticModifier::Readonly));
    EXPECT_EQ(parsed.modifierBits[3], static_cast<std::uint32_t>(SemanticModifier::FunctionScope));
    EXPECT_EQ(parsed.modifierBits[4], 0U);
    EXPECT_FALSE(fl::parseSemanticTokensLegend(Json::object().set("tokenTypes", names({"x"}))).has_value());

    // From the initialize result: full with delta.
    const fl::ServerCapabilities capabilities = fl::parseInitializeResult(Json::object().set(
        "capabilities", Json::object().set("semanticTokensProvider", Json::object()
                                                                         .set("legend", legendJson())
                                                                         .set("full", Json::object().set("delta", true))
                                                                         .set("range", false))));
    const std::optional<fl::SemanticTokensSupport> support = fl::semanticTokensSupport(capabilities);
    ASSERT_TRUE(support.has_value());
    EXPECT_TRUE(support->full);
    EXPECT_TRUE(support->delta);
    EXPECT_EQ(support->legend, parsed);
    EXPECT_FALSE(
        fl::semanticTokensSupport(fl::parseInitializeResult(Json::object().set("capabilities", Json::object())))
            .has_value());
}

TEST(SemanticTokensTest, DecodesARecordedRelativeStream) {
    // int scaled(int factor) {      line 0
    //   int local = factor * SCALE; line 1
    const frost::text::Snapshot snapshot = snapshotOf("int scaled(int factor) {\n  int local = factor * SCALE;\n}\n");
    // A parameter declaration at 0:15, then the local at 1:6, the parameter at 1:14 (8 further on
    // the same line) and the macro at 1:23.
    const std::vector<std::uint32_t> data{
        0, 15, 6, 1, 0b0001, // factor: parameter, declaration
        1, 6,  5, 0, 0b1001, // local: variable, declaration | functionScope
        0, 8,  6, 1, 0,      // factor: parameter
        0, 9,  5, 3, 0,      // SCALE: macro
        7, 0,  3, 0, 0,      // past the last line: dropped
    };
    const auto tokens = fl::decodeSemanticTokens(data, legend(), snapshot, fl::PositionEncoding::Utf8);
    ASSERT_TRUE(tokens.has_value()) << tokens.error().describe();
    ASSERT_EQ(tokens->size(), 4U);
    const std::string text = snapshot.text();
    EXPECT_EQ((*tokens)[0].type, SemanticTokenType::Parameter);
    EXPECT_EQ(snapshot.text((*tokens)[0].range), "factor");
    EXPECT_TRUE(fl::hasModifier((*tokens)[0].modifiers, SemanticModifier::Declaration));
    EXPECT_EQ((*tokens)[1].type, SemanticTokenType::Variable);
    EXPECT_EQ(snapshot.text((*tokens)[1].range), "local");
    EXPECT_TRUE(fl::hasModifier((*tokens)[1].modifiers, SemanticModifier::FunctionScope));
    EXPECT_EQ(snapshot.text((*tokens)[2].range), "factor");
    EXPECT_EQ((*tokens)[2].range, bytes(text.find("factor *"), text.find("factor *") + 6));
    EXPECT_EQ((*tokens)[3].type, SemanticTokenType::Macro);
    EXPECT_EQ(snapshot.text((*tokens)[3].range), "SCALE");

    const std::vector<std::uint32_t> torn{0, 1, 2, 3};
    const auto corrupt = fl::decodeSemanticTokens(torn, legend(), snapshot, fl::PositionEncoding::Utf8);
    ASSERT_FALSE(corrupt.has_value());
    EXPECT_EQ(corrupt.error().code(), frost::ErrorCode::Corrupt);
}

TEST(SemanticTokensTest, DeltaEditsApplyFromTheEndAndRejectBadEdits) {
    const std::vector<std::uint32_t> previous{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<fl::SemanticTokensEdit> edits{{5, 5, {50, 51, 52, 53, 54, 55, 56, 57, 58, 59}}, {0, 1, {}}};
    const auto applied = fl::applySemanticTokensEdits(previous, edits);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, (std::vector<std::uint32_t>{1, 2, 3, 4, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59}));

    const std::vector<fl::SemanticTokensEdit> past{{8, 5, {}}};
    const auto outOfRange = fl::applySemanticTokensEdits(previous, past);
    ASSERT_FALSE(outOfRange.has_value());
    EXPECT_EQ(outOfRange.error().code(), frost::ErrorCode::OutOfRange);
    const std::vector<fl::SemanticTokensEdit> overlapping{{0, 4, {}}, {2, 1, {}}};
    const auto overlap = fl::applySemanticTokensEdits(previous, overlapping);
    ASSERT_FALSE(overlap.has_value());
    EXPECT_EQ(overlap.error().code(), frost::ErrorCode::InvalidArgument);

    const auto parsed = fl::parseSemanticTokensResult(
        Json::object()
            .set("resultId", "7")
            .set("edits",
                 Json::array({Json::object().set("start", 0).set("deleteCount", 1).set("data", integersJson({9}))})));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->resultId, "7");
    ASSERT_TRUE(parsed->edits.has_value());
    EXPECT_EQ(parsed->edits->front().data, std::vector<std::uint32_t>{9});
    EXPECT_FALSE(fl::parseSemanticTokensResult(Json::object().set("resultId", "1")).has_value());
}

TEST(SemanticTokensTest, RequestsFullThenDeltaAndFallsBackToFull) {
    FeatureServer server;
    const std::string path = workPath("main.cpp");
    int fullRequests = 0;
    server.on("textDocument/semanticTokens/full", [&](const Json&) {
        ++fullRequests;
        return Json::object().set("resultId", std::to_string(fullRequests)).set("data", integersJson({0, 4, 5, 0, 0}));
    });
    server.on("textDocument/semanticTokens/full/delta", [](const Json& params) {
        if (params.stringAt("previousResultId") == "1") {
            return Json::object()
                .set("resultId", "2")
                .set(
                    "edits",
                    Json::array({Json::object().set("start", 1).set("deleteCount", 1).set("data", integersJson({6}))}));
        }
        // Edits that do not fit the previous array.
        return Json::object().set(
            "edits",
            Json::array({Json::object().set("start", 40).set("deleteCount", 1).set("data", integersJson({}))}));
    });

    const auto full = server.run(fl::requestSemanticTokens(server.client(), path, std::nullopt, true));
    ASSERT_TRUE(full.has_value()) << full.error().describe();
    EXPECT_EQ(full->resultId, "1");
    EXPECT_EQ(full->data, (std::vector<std::uint32_t>{0, 4, 5, 0, 0}));

    const auto delta = server.run(fl::requestSemanticTokens(server.client(), path, *full, true));
    ASSERT_TRUE(delta.has_value()) << delta.error().describe();
    EXPECT_EQ(delta->resultId, "2");
    EXPECT_EQ(delta->data, (std::vector<std::uint32_t>{0, 6, 5, 0, 0}));
    const std::vector<Json> deltaParams = server.paramsOf("textDocument/semanticTokens/full/delta");
    ASSERT_EQ(deltaParams.size(), 1U);
    EXPECT_EQ(deltaParams[0].stringAt("previousResultId"), "1");
    EXPECT_EQ(deltaParams[0].find("textDocument")->stringAt("uri"), fl::pathToUri(path).value());

    // A delta the client cannot apply turns into one full request.
    const auto fallback = server.run(fl::requestSemanticTokens(server.client(), path, *delta, true));
    ASSERT_TRUE(fallback.has_value()) << fallback.error().describe();
    EXPECT_EQ(fallback->resultId, "2"); // The second full answer.
    EXPECT_EQ(fullRequests, 2);
    // Without delta support every request is full.
    const auto noDelta = server.run(fl::requestSemanticTokens(server.client(), path, *fallback, false));
    ASSERT_TRUE(noDelta.has_value());
    EXPECT_EQ(fullRequests, 3);
}

TEST(SemanticTokensTest, InactiveRegionsDecodeAndMapToTheSnapshot) {
    const std::string path = workPath("main.cpp");
    const frost::text::Snapshot snapshot =
        snapshotOf("#ifdef FROST_NOT_DEFINED\nint disabledCode;\n#endif\nint live;\n");
    const Json params =
        Json::object()
            .set("textDocument", Json::object().set("uri", fl::pathToUri(path).value()))
            .set("regions", Json::array({fl::toJson(fl::LspRange{{1, 0}, {1, 17}}),
                                         fl::toJson(fl::LspRange{{9, 0}, {12, 0}})})); // Past the end: dropped.
    auto regions = fl::decodeInactiveRegions(params);
    ASSERT_TRUE(regions.has_value()) << regions.error().describe();
    EXPECT_EQ(fl::documentKey(regions->file), fl::documentKey(path));
    ASSERT_EQ(regions->lspRanges.size(), 2U);
    fl::mapInactiveRegions(*regions, snapshot, fl::PositionEncoding::Utf8);
    ASSERT_EQ(regions->ranges.size(), 1U);
    EXPECT_EQ(snapshot.text(regions->ranges[0]), "int disabledCode;");
    EXPECT_EQ(regions->textVersion, snapshot.version());

    EXPECT_FALSE(fl::decodeInactiveRegions(Json::object().set("regions", Json::array())).has_value());
}

TEST(SemanticTokensTest, ClientAnnouncesSemanticTokensAndInactiveRegions) {
    const Json capabilities = fl::clientCapabilities();
    const Json* document = capabilities.find("textDocument");
    ASSERT_NE(document, nullptr);
    const Json* tokens = document->find("semanticTokens");
    ASSERT_NE(tokens, nullptr);
    EXPECT_EQ(tokens->find("requests")->find("full")->boolAt("delta"), true);
    EXPECT_EQ(tokens->find("formats")->size(), 1U);
    bool hasConcept = false;
    for (const Json& name : tokens->find("tokenTypes")->items()) {
        hasConcept = hasConcept || (name.asString() != nullptr && *name.asString() == "concept");
    }
    EXPECT_TRUE(hasConcept);
    EXPECT_EQ(document->find("inactiveRegionsCapabilities")->boolAt("inactiveRegions"), true);
}

} // namespace
