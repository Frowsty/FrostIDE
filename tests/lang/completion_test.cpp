// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for code completion (completion.hpp): decoding clangd-shaped items, the auto-popup
// triggers, the model's filtering, Rider-like order and selection, commits with snippets and
// additional edits, and the completion and resolve requests against the scripted server.

#include "frost/lang/completion.hpp"
#include "frost/text/text_buffer.hpp"

#include "lang/feature_server.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::lang::autoCompletionTrigger;
using frost::lang::CompletionItem;
using frost::lang::CompletionItemKind;
using frost::lang::CompletionList;
using frost::lang::CompletionModel;
using frost::lang::CompletionTrigger;
using frost::lang::CompletionTriggerKind;
using frost::lang::Json;
using frost::lang::LspPosition;
using frost::lang::LspRange;
using frost::lang::PositionEncoding;
using frost::test::FeatureServer;
using frost::test::workPath;

Json range(std::uint32_t line, std::uint32_t from, std::uint32_t to) {
    return frost::lang::toJson(LspRange{{line, from}, {line, to}});
}

// A clangd-shaped completion item.
Json item(std::string_view label, std::string_view filter, std::string_view sort, int kind = 3) {
    return Json::object()
        .set("label", std::string(" ") + std::string(label))
        .set("filterText", filter)
        .set("sortText", sort)
        .set("kind", kind)
        .set("insertTextFormat", 1)
        .set("textEdit", Json::object().set("range", range(0, 0, 0)).set("newText", filter));
}

CompletionItem plain(const std::string& label, const std::string& sort = {}, bool deprecated = false) {
    CompletionItem out;
    out.label = label;
    out.filterText = label;
    out.sortText = sort.empty() ? label : sort;
    out.insertText = label;
    out.deprecated = deprecated;
    return out;
}

frost::text::Snapshot snapshotOf(std::string text) {
    return frost::text::TextBuffer::fromUtf8(std::move(text)).value().snapshot();
}

TEST(Completion, DecodesClangdItems) {
    const Json json =
        Json::object()
            .set("label", "\xE2\x80\xA2"
                          "addOne(int x)")
            .set("filterText", "addOne")
            .set("sortText", "3f00001aaddOne")
            .set("detail", "int")
            .set("kind", 3)
            .set("insertTextFormat", 2)
            .set("documentation", Json::object().set("kind", "markdown").set("value", "Adds **one**."))
            .set("textEdit", Json::object().set("range", range(2, 4, 7)).set("newText", "addOne(${1:int x})"))
            .set("additionalTextEdits",
                 Json::array({Json::object().set("range", range(0, 0, 0)).set("newText", "#include \"math.h\"\n")}))
            .set("score", 2.5);
    const CompletionItem decoded = frost::lang::parseCompletionItem(json);
    EXPECT_EQ(decoded.label, "addOne(int x)");
    EXPECT_EQ(decoded.filterText, "addOne");
    EXPECT_EQ(decoded.kind, CompletionItemKind::Function);
    EXPECT_TRUE(decoded.snippet);
    EXPECT_EQ(decoded.insertText, "addOne(${1:int x})");
    ASSERT_TRUE(decoded.editRange.has_value());
    EXPECT_EQ(decoded.editRange->start, (LspPosition{2, 4}));
    ASSERT_EQ(decoded.additionalTextEdits.size(), 1u);
    EXPECT_TRUE(decoded.documentation.markdown);
    EXPECT_EQ(decoded.documentation.text, "Adds **one**.");
    EXPECT_DOUBLE_EQ(decoded.score, 2.5);

    const CompletionList list = frost::lang::parseCompletionList(
        Json::object().set("isIncomplete", true).set("items", Json::array({json, json})));
    EXPECT_TRUE(list.isIncomplete);
    EXPECT_EQ(list.items.size(), 2u);
    EXPECT_EQ(frost::lang::parseCompletionList(Json::array({json})).items.size(), 1u);
    EXPECT_TRUE(frost::lang::parseCompletionList(Json()).items.empty());
}

TEST(Completion, AutoPopupTriggers) {
    const CompletionTrigger invoked{};
    const auto character = [](std::string c) {
        return CompletionTrigger{.kind = CompletionTriggerKind::TriggerCharacter, .character = std::move(c)};
    };
    EXPECT_EQ(autoCompletionTrigger("    v"), invoked);
    EXPECT_EQ(autoCompletionTrigger("x = f"), invoked);
    EXPECT_FALSE(autoCompletionTrigger("    va").has_value()); // Already inside a word.
    EXPECT_FALSE(autoCompletionTrigger("x = 1").has_value());  // Numbers do not complete.
    EXPECT_EQ(autoCompletionTrigger("point."), character("."));
    EXPECT_EQ(autoCompletionTrigger("get()."), character("."));
    EXPECT_FALSE(autoCompletionTrigger("x = 1.").has_value());
    EXPECT_EQ(autoCompletionTrigger("ptr->"), character(">"));
    EXPECT_FALSE(autoCompletionTrigger("a > ").has_value());
    EXPECT_EQ(autoCompletionTrigger("std::"), character(":"));
    EXPECT_FALSE(autoCompletionTrigger("case 1:").has_value());
    EXPECT_EQ(autoCompletionTrigger("#include <"), character("<"));
    EXPECT_EQ(autoCompletionTrigger("#include \"sub/"), character("/"));
    EXPECT_FALSE(autoCompletionTrigger("if (a < ").has_value());
    EXPECT_EQ(frost::lang::identifierStart("x = fooBa"), 4u);
}

TEST(Completion, ModelFiltersAndSortsLikeRider) {
    CompletionModel model;
    CompletionList list;
    list.items = {plain("getValue", "2"),     plain("GetValueImpl", "1"),
                  plain("gravityValue", "0"), plain("getValueOld", "0", /*deprecated=*/true),
                  plain("value", "0"),        plain("goToVoid", "0")};
    model.setList(std::move(list));
    EXPECT_EQ(model.entries().size(), 6u); // An empty prefix keeps everything.

    model.setPrefix("getV");
    std::vector<std::string> order;
    for (const auto& entry : model.entries()) {
        order.push_back(model.items()[entry.item].label);
    }
    // Case-sensitive prefixes (deprecated last), then the case-insensitive prefix; "value" and
    // "goToVoid" do not match.
    EXPECT_EQ(order, (std::vector<std::string>{"getValue", "getValueOld", "GetValueImpl"}));

    model.setPrefix("gV");
    order.clear();
    for (const auto& entry : model.entries()) {
        order.push_back(model.items()[entry.item].label);
    }
    // All word-start matches: the exact-case hump first, then equal scores by sortText and the
    // shorter label, the deprecated item last.
    EXPECT_EQ(order, (std::vector<std::string>{"getValue", "goToVoid", "gravityValue", "GetValueImpl", "getValueOld"}));

    ASSERT_TRUE(model.selected().has_value());
    EXPECT_EQ(*model.selected(), 0u);
    model.moveSelection(-1);
    EXPECT_EQ(*model.selected(), model.entries().size() - 1); // Wraps.
    model.moveSelection(1);
    EXPECT_EQ(*model.selected(), 0u);

    model.setPrefix("zzz");
    EXPECT_TRUE(model.empty());
    EXPECT_EQ(model.selectedItem(), nullptr);
}

TEST(Completion, CommitsSnippetsWithAdditionalEdits) {
    // Line 0 is where clangd inserts the include; the user typed "ad" on line 2 and then "dO".
    const auto snapshot = snapshotOf("#pragma once\n\nint r = addO\n");
    CompletionItem chosen;
    chosen.label = "addOne(int x)";
    chosen.snippet = true;
    chosen.insertText = "addOne(${1:int x})$0";
    chosen.editRange = LspRange{{2, 8}, {2, 10}}; // Computed when only "ad" was typed.
    chosen.additionalTextEdits = {{.range = LspRange{{1, 0}, {1, 0}}, .newText = "#include \"math.h\"\n"}};
    const ByteOffset wordStart{14 + 8};
    const ByteOffset caret{14 + 12};
    auto commit = frost::lang::commitCompletion(chosen, snapshot, wordStart, caret, PositionEncoding::Utf8);
    ASSERT_TRUE(commit.has_value()) << commit.error().describe();
    ASSERT_EQ(commit->edits.size(), 2u);
    EXPECT_EQ(commit->edits[0].text, "#include \"math.h\"\n");
    EXPECT_EQ(commit->edits[1].range.begin, wordStart);
    EXPECT_EQ(commit->edits[1].range.end, caret); // Stretched to the caret.
    EXPECT_EQ(commit->edits[1].text, "addOne(int x)");

    // Apply and check where the caret and the tab stop land.
    auto buffer = frost::text::TextBuffer::fromUtf8(snapshot.text()).value();
    std::vector<frost::text::Edit> edits;
    for (const auto& edit : commit->edits) {
        edits.push_back({edit.range, edit.text});
    }
    ASSERT_TRUE(buffer.applyEdits(edits).has_value());
    const std::string after = buffer.current().text();
    EXPECT_EQ(after, "#pragma once\n#include \"math.h\"\n\nint r = addOne(int x)\n");
    ASSERT_EQ(commit->tabStops.size(), 2u);
    EXPECT_EQ(after.substr(commit->tabStops[0].begin.value(), commit->tabStops[0].size()), "int x");
    EXPECT_EQ(commit->caret, commit->tabStops[0].begin);
    EXPECT_EQ(after.substr(commit->tabStops[1].begin.value(), 1), "\n"); // $0 after the call.
}

TEST(Completion, CommitWithoutEditRangeReplacesTheTypedWord) {
    const auto snapshot = snapshotOf("x.leng");
    CompletionItem chosen = plain("length");
    auto commit = frost::lang::commitCompletion(chosen, snapshot, ByteOffset{2}, ByteOffset{6}, PositionEncoding::Utf8);
    ASSERT_TRUE(commit.has_value());
    ASSERT_EQ(commit->edits.size(), 1u);
    EXPECT_EQ(commit->edits[0].range.begin, ByteOffset{2});
    EXPECT_EQ(commit->edits[0].text, "length");
    EXPECT_EQ(commit->caret, ByteOffset{8});
    EXPECT_TRUE(commit->tabStops.empty());
}

TEST(Completion, RequestsWithContextAndResolvesOnlyWhenSupported) {
    FeatureServer server;
    server.on("textDocument/completion", [](const Json&) {
        return Json::object().set("isIncomplete", false).set("items", Json::array({item("size()", "size", "1")}));
    });
    const std::string path = workPath("main.cpp");
    auto list = server.run(frost::lang::requestCompletion(
        server.client(), path, LspPosition{3, 6},
        CompletionTrigger{.kind = CompletionTriggerKind::TriggerCharacter, .character = "."}));
    ASSERT_TRUE(list.has_value()) << list.error().describe();
    ASSERT_EQ(list->items.size(), 1u);
    EXPECT_EQ(list->items[0].label, "size()");
    const auto sent = server.paramsOf("textDocument/completion");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].find("position")->integerAt("line"), 3);
    EXPECT_EQ(sent[0].find("context")->integerAt("triggerKind"), 2);
    EXPECT_EQ(sent[0].find("context")->stringAt("triggerCharacter"), ".");

    // resolveProvider is false: no request, the item comes back marked resolved.
    auto resolved = server.run(frost::lang::resolveCompletionItem(server.client(), list->items[0]));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->resolved);
    EXPECT_TRUE(server.paramsOf("completionItem/resolve").empty());
}

TEST(Completion, ResolveFetchesDocumentation) {
    Json capabilities = FeatureServer::defaultCapabilities();
    capabilities.set("completionProvider", Json::object().set("resolveProvider", true));
    FeatureServer server(capabilities);
    server.on("completionItem/resolve", [](const Json& params) {
        Json out = params;
        out.set("documentation", Json::object().set("kind", "markdown").set("value", "Returns the size."));
        return out;
    });
    CompletionItem chosen = frost::lang::parseCompletionItem(item("size()", "size", "1"));
    auto resolved = server.run(frost::lang::resolveCompletionItem(server.client(), chosen));
    ASSERT_TRUE(resolved.has_value()) << resolved.error().describe();
    EXPECT_EQ(resolved->documentation.text, "Returns the size.");
    EXPECT_EQ(resolved->label, "size()");
    EXPECT_TRUE(resolved->resolved);
    EXPECT_EQ(server.paramsOf("completionItem/resolve").size(), 1u);
}

} // namespace
