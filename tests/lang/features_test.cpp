// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the navigation and editing feature models against the scripted server
// (code_assist.hpp, navigation.hpp, symbols.hpp, refactoring.hpp): signature help and its
// parameter ranges, hover markup, Rider's go to declaration or usages, switch header/source,
// find usages grouped with line text, document highlights, the symbol tree with speed search,
// workspace symbols, rename, code actions and formatting.

#include "frost/io/mem_fs.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "lang/feature_server.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::lang::Json;
using frost::lang::LspPosition;
using frost::lang::LspRange;
using frost::lang::PositionEncoding;
using frost::test::FeatureServer;
using frost::test::workPath;

Json rangeJson(std::uint32_t line, std::uint32_t from, std::uint32_t to) {
    return frost::lang::toJson(LspRange{{line, from}, {line, to}});
}

std::string uriOf(const std::string& path) { return frost::lang::pathToUri(path).value(); }

Json location(const std::string& path, std::uint32_t line, std::uint32_t from, std::uint32_t to) {
    return Json::object().set("uri", uriOf(path)).set("range", rangeJson(line, from, to));
}

TEST(SignatureHelpModel, DecodesAndFindsParameterRanges) {
    FeatureServer server;
    server.on("textDocument/signatureHelp", [](const Json&) {
        return Json::object()
            .set("signatures",
                 Json::array({Json::object()
                                  .set("label", "int pick(int x, int x)")
                                  .set("documentation", "Picks one.")
                                  .set("parameters", Json::array({Json::object().set("label", "int x"),
                                                                  Json::object().set("label", "int x")}))}))
            .set("activeSignature", 0)
            .set("activeParameter", 1);
    });
    auto help = server.run(
        frost::lang::requestSignatureHelp(server.client(), workPath("a.cpp"), LspPosition{1, 9}, '(', false));
    ASSERT_TRUE(help.has_value()) << help.error().describe();
    ASSERT_TRUE(help->has_value());
    const auto& signature = (*help)->signatures.at(0);
    EXPECT_EQ((*help)->activeParameterOf(0), 1u);
    const auto first = frost::lang::parameterRange(signature, 0);
    const auto second = frost::lang::parameterRange(signature, 1);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->first, 9u);
    EXPECT_EQ(second->first, 16u); // The second "int x", not the first again.
    EXPECT_EQ(signature.label.substr(second->first, second->second - second->first), "int x");
    const auto sent = server.paramsOf("textDocument/signatureHelp");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].find("context")->stringAt("triggerCharacter"), "(");

    server.on("textDocument/signatureHelp", [](const Json&) { return Json(); });
    auto none = server.run(
        frost::lang::requestSignatureHelp(server.client(), workPath("a.cpp"), LspPosition{1, 9}, std::nullopt, false));
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
    EXPECT_TRUE(frost::lang::isSignatureHelpTrigger(','));
    EXPECT_FALSE(frost::lang::isSignatureHelpTrigger(')'));
}

TEST(HoverModel, ReadsMarkupAndMarkedStrings) {
    FeatureServer server;
    server.on("textDocument/hover", [](const Json&) {
        return Json::object()
            .set("contents", Json::object().set("kind", "markdown").set("value", "### function `add`\n---\nAdds."))
            .set("range", rangeJson(2, 4, 7));
    });
    auto hover = server.run(frost::lang::requestHover(server.client(), workPath("a.cpp"), LspPosition{2, 5}));
    ASSERT_TRUE(hover.has_value() && hover->has_value());
    EXPECT_TRUE((*hover)->contents.markdown);
    EXPECT_EQ((*hover)->contents.text, "### function `add`\n---\nAdds.");
    ASSERT_TRUE((*hover)->range.has_value());

    const auto marked = frost::lang::parseHover(Json::object().set(
        "contents", Json::array({Json::object().set("language", "cpp").set("value", "int add(int)"), Json("Adds.")})));
    ASSERT_TRUE(marked.has_value());
    EXPECT_EQ(marked->contents.text, "```cpp\nint add(int)\n```\n\nAdds.");
    EXPECT_FALSE(frost::lang::parseHover(Json::object().set("contents", "")).has_value());
}

TEST(NavigationModel, GoToDeclarationOrUsages) {
    FeatureServer server;
    const std::string source = workPath("a.cpp");
    const std::string header = workPath("a.h");
    // From a use: the definition elsewhere.
    server.on("textDocument/definition", [&](const Json&) { return Json::array({location(source, 10, 4, 7)}); });
    auto use = server.run(frost::lang::gotoDeclarationOrUsages(server.client(), source, LspPosition{20, 5}));
    ASSERT_TRUE(use.has_value()) << use.error().describe();
    ASSERT_EQ(use->locations.size(), 1u);
    EXPECT_EQ(use->locations[0].range.start.line, 10u);
    EXPECT_FALSE(use->showUsages);

    // On the definition: the declaration in the header (a LocationLink).
    server.on("textDocument/declaration", [&](const Json&) {
        return Json::array({Json::object()
                                .set("targetUri", uriOf(header))
                                .set("targetRange", rangeJson(3, 0, 20))
                                .set("targetSelectionRange", rangeJson(3, 4, 7))});
    });
    auto definition = server.run(frost::lang::gotoDeclarationOrUsages(server.client(), source, LspPosition{10, 5}));
    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ(definition->locations.size(), 1u);
    EXPECT_EQ(frost::lang::documentKey(definition->locations[0].path), frost::lang::documentKey(header));
    EXPECT_EQ(definition->locations[0].range.start.character, 4u);

    // On a declaration that is its own definition: find usages.
    server.on("textDocument/declaration", [&](const Json&) { return Json::array({location(source, 10, 4, 7)}); });
    auto self = server.run(frost::lang::gotoDeclarationOrUsages(server.client(), source, LspPosition{10, 6}));
    ASSERT_TRUE(self.has_value());
    EXPECT_TRUE(self->showUsages);
    EXPECT_TRUE(self->locations.empty());

    server.on("textDocument/implementation", [&](const Json&) { return Json::array({location(source, 30, 0, 3)}); });
    auto implementations = server.run(frost::lang::requestLocations(
        server.client(), frost::lang::NavigationTarget::Implementation, source, LspPosition{1, 1}));
    ASSERT_TRUE(implementations.has_value());
    EXPECT_EQ(implementations->size(), 1u);
}

TEST(NavigationModel, SwitchesBetweenHeaderAndSource) {
    FeatureServer server;
    const std::string header = workPath("a.h");
    server.on("textDocument/switchSourceHeader", [&](const Json&) { return Json(uriOf(header)); });
    auto counterpart = server.run(frost::lang::requestSwitchSourceHeader(server.client(), workPath("a.cpp")));
    ASSERT_TRUE(counterpart.has_value() && counterpart->has_value());
    EXPECT_EQ(frost::lang::documentKey(**counterpart), frost::lang::documentKey(header));
    const auto sent = server.paramsOf("textDocument/switchSourceHeader");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].stringAt("uri"), uriOf(workPath("a.cpp")));

    server.on("textDocument/switchSourceHeader", [](const Json&) { return Json(); });
    auto none = server.run(frost::lang::requestSwitchSourceHeader(server.client(), workPath("lonely.cpp")));
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
}

TEST(NavigationModel, FindUsagesGroupsByFileWithLineText) {
    FeatureServer server;
    const std::string a = workPath("a.cpp");
    const std::string b = workPath("b.cpp");
    server.on("textDocument/references", [&](const Json&) {
        return Json::array({location(b, 1, 8, 13), location(a, 2, 4, 9), location(a, 0, 4, 9)});
    });
    auto found = server.run(frost::lang::requestReferences(server.client(), a, LspPosition{0, 5}));
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    EXPECT_EQ(server.paramsOf("textDocument/references")[0].find("context")->boolAt("includeDeclaration"), true);

    frost::io::MemFs fs(frost::systemClock());
    ASSERT_TRUE(fs.createDirectories(std::string(frost::io::parentPath(a))).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(b, "// b\n    int total = value();\n").has_value());
    // a.cpp is "open": its text comes from the snapshot, not the disk (where it does not exist).
    const auto openA = frost::text::TextBuffer::fromUtf8("int value();\nint main() {\n    value();\n}\n").value();
    frost::lang::FileLineSource lines(fs, [&](std::string_view path) -> std::optional<frost::text::Snapshot> {
        if (frost::lang::documentKey(path) == frost::lang::documentKey(a)) {
            return openA.snapshot();
        }
        return std::nullopt;
    });
    const auto groups = frost::lang::groupUsages(*found, lines.source(), PositionEncoding::Utf8);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(frost::lang::documentKey(groups[0].path), frost::lang::documentKey(a));
    ASSERT_EQ(groups[0].usages.size(), 2u);
    EXPECT_EQ(groups[0].usages[0].line, 1u);
    EXPECT_EQ(groups[0].usages[0].lineText, "int value();");
    EXPECT_EQ(groups[0].usages[1].line, 3u);
    EXPECT_EQ(groups[0].usages[1].lineText, "value();"); // Indentation trimmed.
    EXPECT_EQ(groups[0].usages[1].highlightBegin, 0u);
    EXPECT_EQ(groups[0].usages[1].highlightEnd, 5u);
    ASSERT_EQ(groups[1].usages.size(), 1u);
    EXPECT_EQ(groups[1].usages[0].lineText, "int total = value();");
    EXPECT_EQ(groups[1].usages[0].column, 9u);
}

TEST(NavigationModel, DocumentHighlightsAreSortedWithKinds) {
    FeatureServer server;
    server.on("textDocument/documentHighlight", [](const Json&) {
        return Json::array({Json::object().set("range", rangeJson(4, 0, 3)).set("kind", 3),
                            Json::object().set("range", rangeJson(1, 2, 5)).set("kind", 2)});
    });
    auto highlights =
        server.run(frost::lang::requestDocumentHighlights(server.client(), workPath("a.cpp"), LspPosition{1, 3}));
    ASSERT_TRUE(highlights.has_value());
    ASSERT_EQ(highlights->size(), 2u);
    EXPECT_EQ((*highlights)[0].range.start.line, 1u);
    EXPECT_EQ((*highlights)[0].kind, frost::lang::HighlightKind::Read);
    EXPECT_EQ((*highlights)[1].kind, frost::lang::HighlightKind::Write);
}

TEST(SymbolsModel, FileStructureTreeWithSpeedSearch) {
    FeatureServer server;
    const auto symbol = [](std::string name, int kind, std::uint32_t line, Json children = Json::array()) {
        return Json::object()
            .set("name", name)
            .set("kind", kind)
            .set("range", rangeJson(line, 0, 40))
            .set("selectionRange", rangeJson(line, 6, 6 + static_cast<std::uint32_t>(name.size())))
            .set("children", std::move(children));
    };
    server.on("textDocument/documentSymbol", [&](const Json&) {
        return Json::array({symbol("Widget", 5, 1, Json::array({symbol("paint", 6, 2), symbol("resize", 6, 3)})),
                            symbol("main", 12, 10)});
    });
    auto symbols = server.run(frost::lang::requestDocumentSymbols(server.client(), workPath("a.cpp")));
    ASSERT_TRUE(symbols.has_value()) << symbols.error().describe();
    ASSERT_EQ(symbols->size(), 2u);
    EXPECT_EQ((*symbols)[0].kind, frost::lang::SymbolKind::Class);
    ASSERT_EQ((*symbols)[0].children.size(), 2u);
    EXPECT_EQ(frost::lang::flattenSymbols(*symbols).size(), 4u);

    const auto filtered = frost::lang::filterSymbols(*symbols, "pai");
    ASSERT_EQ(filtered.size(), 1u); // Widget stays as the context of paint.
    EXPECT_FALSE(filtered[0].matched);
    ASSERT_EQ(filtered[0].children.size(), 1u);
    EXPECT_EQ(filtered[0].children[0].name, "paint");
    EXPECT_TRUE(filtered[0].children[0].matched);
    EXPECT_EQ(frost::lang::filterSymbols(*symbols, "").size(), 2u);

    // The flat SymbolInformation form.
    const auto flat = frost::lang::parseDocumentSymbols(Json::array(
        {Json::object().set("name", "f").set("kind", 12).set("location", location(workPath("a.cpp"), 5, 4, 5))}));
    ASSERT_EQ(flat.size(), 1u);
    EXPECT_EQ(flat[0].selectionRange.start.line, 5u);
}

TEST(SymbolsModel, GoToSymbolRanksMatches) {
    FeatureServer server;
    server.on("workspace/symbol", [](const Json&) {
        const auto info = [](std::string name, std::string container) {
            return Json::object()
                .set("name", name)
                .set("containerName", container)
                .set("kind", 12)
                .set("location", location(workPath("x.cpp"), 1, 0, 1));
        };
        return Json::array(
            {info("getPlayerHealth", "game"), info("gpHelper", "util"), info("unrelated", ""), info("gph", "")});
    });
    auto symbols = server.run(frost::lang::requestWorkspaceSymbols(server.client(), "gph"));
    ASSERT_TRUE(symbols.has_value()) << symbols.error().describe();
    std::vector<std::string> names;
    for (const auto& symbol : *symbols) {
        names.push_back(symbol.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"gph", "gpHelper", "getPlayerHealth"}));
    EXPECT_EQ(server.paramsOf("workspace/symbol")[0].stringAt("query"), "gph");
}

TEST(RefactoringModel, PrepareRenameThenRename) {
    FeatureServer server;
    const std::string a = workPath("a.cpp");
    const std::string h = workPath("a.h");
    server.on("textDocument/prepareRename",
              [](const Json&) { return Json::object().set("range", rangeJson(3, 4, 9)).set("placeholder", "value"); });
    server.on("textDocument/rename", [&](const Json&) {
        return Json::object().set(
            "changes",
            Json::object()
                .set(uriOf(a), Json::array({Json::object().set("range", rangeJson(3, 4, 9)).set("newText", "amount")}))
                .set(uriOf(h),
                     Json::array({Json::object().set("range", rangeJson(1, 4, 9)).set("newText", "amount")})));
    });
    auto prepared = server.run(frost::lang::requestPrepareRename(server.client(), a, LspPosition{3, 5}));
    ASSERT_TRUE(prepared.has_value() && prepared->has_value());
    EXPECT_EQ((*prepared)->placeholder, "value");
    EXPECT_EQ((*prepared)->range.start.character, 4u);
    auto edit = server.run(frost::lang::requestRename(server.client(), a, LspPosition{3, 5}, "amount"));
    ASSERT_TRUE(edit.has_value()) << edit.error().describe();
    EXPECT_EQ(edit->files.size(), 2u);
    EXPECT_EQ(edit->editCount(), 2u);
    EXPECT_EQ(server.paramsOf("textDocument/rename")[0].stringAt("newName"), "amount");

    server.on("textDocument/prepareRename", [](const Json&) { return Json(); });
    auto refused = server.run(frost::lang::requestPrepareRename(server.client(), a, LspPosition{0, 0}));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->has_value());
    server.on("textDocument/prepareRename", [](const Json&) { return Json::object().set("__error", "not a symbol"); });
    auto failed = server.run(frost::lang::requestPrepareRename(server.client(), a, LspPosition{0, 0}));
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().message().find("not a symbol"), std::string::npos);
    auto empty = server.run(frost::lang::requestRename(server.client(), a, LspPosition{3, 5}, ""));
    EXPECT_FALSE(empty.has_value());
}

TEST(RefactoringModel, QuickFixesSendDiagnosticsAndKeepEdits) {
    FeatureServer server;
    const std::string a = workPath("a.cpp");
    server.on("textDocument/codeAction", [&](const Json&) {
        const auto fix = [&](std::string title, std::string kind, bool preferred) {
            return Json::object()
                .set("title", title)
                .set("kind", kind)
                .set("isPreferred", preferred)
                .set("edit",
                     Json::object().set(
                         "changes",
                         Json::object().set(
                             uriOf(a),
                             Json::array({Json::object().set("range", rangeJson(2, 4, 4)).set("newText", ";")}))));
        };
        return Json::array({fix("Extract to function", "refactor.extract", false), fix("Insert ';'", "quickfix", true),
                            Json::object().set("title", "Run tweak").set("command", "clangd.applyTweak")});
    });
    frost::lang::LiveDiagnostic diagnostic;
    diagnostic.message = "expected ';'";
    diagnostic.code = "expected_semi";
    diagnostic.source = "clang";
    diagnostic.lspRange = LspRange{{2, 4}, {2, 4}};
    frost::lang::LiveDiagnostic elsewhere = diagnostic;
    elsewhere.lspRange = LspRange{{9, 0}, {9, 1}};
    const std::vector<frost::lang::LiveDiagnostic> all{diagnostic, elsewhere};
    const auto atCaret = frost::lang::diagnosticsAt(all, LspRange{{2, 4}, {2, 4}});
    ASSERT_EQ(atCaret.size(), 1u);
    auto actions = server.run(frost::lang::requestCodeActions(server.client(), a, LspRange{{2, 4}, {2, 4}}, atCaret));
    ASSERT_TRUE(actions.has_value()) << actions.error().describe();
    ASSERT_EQ(actions->size(), 3u); // Command-only tweaks are preserved for workspace/executeCommand.
    EXPECT_EQ((*actions)[0].title, "Insert ';'");
    EXPECT_TRUE((*actions)[0].preferred);
    EXPECT_EQ((*actions)[0].edit.editCount(), 1u);
    EXPECT_EQ((*actions)[2].title, "Run tweak");
    EXPECT_EQ((*actions)[2].commandName, "clangd.applyTweak");
    const auto sent = server.paramsOf("textDocument/codeAction");
    ASSERT_EQ(sent.size(), 1u);
    const Json* diagnostics = sent[0].find("context")->find("diagnostics");
    ASSERT_EQ(diagnostics->size(), 1u);
    EXPECT_EQ(diagnostics->items()[0].stringAt("message"), "expected ';'");
    EXPECT_EQ(diagnostics->items()[0].stringAt("code"), "expected_semi");
}

TEST(RefactoringModel, ReformatsTheFileOrARange) {
    FeatureServer server;
    const auto answer = [](const Json&) {
        return Json::array({Json::object().set("range", rangeJson(0, 3, 5)).set("newText", " ")});
    };
    server.on("textDocument/formatting", answer);
    server.on("textDocument/rangeFormatting", answer);
    frost::lang::FormattingOptions options{.tabSize = 2, .insertSpaces = true};
    auto whole = server.run(frost::lang::requestFormatting(server.client(), workPath("a.cpp"), std::nullopt, options));
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->size(), 1u);
    auto part = server.run(
        frost::lang::requestFormatting(server.client(), workPath("a.cpp"), LspRange{{0, 0}, {3, 0}}, options));
    ASSERT_TRUE(part.has_value());
    EXPECT_EQ(server.paramsOf("textDocument/formatting")[0].find("options")->integerAt("tabSize"), 2);
    ASSERT_EQ(server.paramsOf("textDocument/rangeFormatting").size(), 1u);
    EXPECT_NE(server.paramsOf("textDocument/rangeFormatting")[0].find("range"), nullptr);
}

} // namespace
