// SPDX-License-Identifier: Apache-2.0
//
// EditorInsightBinding offscreen against the scripted language server (ADR 0047): the
// translations from frost::lang's and frost::build's vocabulary to the editor's, semantic tokens
// requested on attach and after an edit (as a delta naming the previous result), clangd's
// "comment" tokens standing in for inactive regions until the extension's notification arrives,
// and live and build diagnostics reaching the view with the analysing state between an edit and
// fresh diagnostics.

#include "frost/lang/uri.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"

#include "editor/editor_harness.hpp"
#include "lang/feature_server.hpp"
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QThread>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
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
using frost::ui::InsightSeverity;
using frost::ui::SemanticKind;
using frost::ui::lang::EditorInsightBinding;

bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

Json names(std::initializer_list<const char*> list) {
    Json out = Json::array();
    for (const char* name : list) {
        out.push(Json(name));
    }
    return out;
}

Json integers(const std::vector<int>& values) {
    Json out = Json::array();
    for (const int value : values) {
        out.push(Json(value));
    }
    return out;
}

// FeatureServer's capabilities plus clangd-like semantic tokens with deltas.
Json capabilities() {
    return FeatureServer::defaultCapabilities().set(
        "semanticTokensProvider",
        Json::object()
            .set("legend", Json::object()
                               .set("tokenTypes", names({"variable", "parameter", "comment"}))
                               .set("tokenModifiers", names({"declaration", "functionScope"})))
            .set("full", Json::object().set("delta", true)));
}

frost::text::ByteRange find(std::string_view text, std::string_view word, std::size_t nth = 0) {
    std::size_t at = text.find(word);
    for (std::size_t i = 0; i < nth && at != std::string_view::npos; ++i) {
        at = text.find(word, at + 1);
    }
    EXPECT_NE(at, std::string_view::npos) << word;
    return frost::text::ByteRange{ByteOffset{at}, ByteOffset{at + word.size()}};
}

frost::ui::test::HarnessOptions at(std::string path) {
    frost::ui::test::HarnessOptions options;
    options.path = std::move(path);
    return options;
}

TEST(EditorInsightTranslation, TokensSeveritiesAndDiagnosticsMapToTheEditorsVocabulary) {
    using frost::ui::lang::semanticKindFor;
    const auto bit = [](SemanticModifier m) { return static_cast<std::uint32_t>(m); };
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Variable, bit(SemanticModifier::FunctionScope)),
              SemanticKind::LocalVariable);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Variable, bit(SemanticModifier::FileScope)),
              SemanticKind::GlobalVariable);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Variable, bit(SemanticModifier::ClassScope)),
              SemanticKind::StaticField);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Property, 0), SemanticKind::Field);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Property, bit(SemanticModifier::Static)), SemanticKind::StaticField);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Method, bit(SemanticModifier::Static)), SemanticKind::StaticMethod);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Struct, 0), SemanticKind::Class);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Type, 0), SemanticKind::TypeAlias);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::TypeParameter, 0), SemanticKind::TemplateParameter);
    EXPECT_EQ(semanticKindFor(SemanticTokenType::Macro, 0), SemanticKind::Macro);
    EXPECT_FALSE(semanticKindFor(SemanticTokenType::Keyword, 0).has_value());
    EXPECT_EQ(
        frost::ui::lang::semanticModifiersFor(bit(SemanticModifier::Readonly) | bit(SemanticModifier::Deprecated)),
        frost::ui::semantic_modifiers::kReadonly | frost::ui::semantic_modifiers::kDeprecated);

    EXPECT_EQ(frost::ui::lang::toInsightSeverity(fl::DiagnosticSeverity::Hint), InsightSeverity::WeakWarning);
    EXPECT_EQ(frost::ui::lang::toInsightSeverity(fl::DiagnosticSeverity::Information), InsightSeverity::Info);
    EXPECT_EQ(frost::ui::lang::toInsightSeverity(frost::build::Severity::Message), InsightSeverity::Info);

    const std::vector<fl::SemanticToken> tokens{
        {frost::text::ByteRange{ByteOffset{0}, ByteOffset{5}}, SemanticTokenType::Comment, 0},
        {frost::text::ByteRange{ByteOffset{6}, ByteOffset{10}}, SemanticTokenType::Comment, 0}, // The next line.
        {frost::text::ByteRange{ByteOffset{11}, ByteOffset{12}}, SemanticTokenType::Variable, 0},
        {frost::text::ByteRange{ByteOffset{20}, ByteOffset{25}}, SemanticTokenType::Comment, 0},
    };
    EXPECT_EQ(frost::ui::lang::inactiveRangesFromTokens(tokens),
              (std::vector<frost::text::ByteRange>{{ByteOffset{0}, ByteOffset{10}}, {ByteOffset{20}, ByteOffset{25}}}));
    const std::vector<frost::ui::SemanticSpan> spans = frost::ui::lang::toSemanticSpans(tokens);
    ASSERT_EQ(spans.size(), 1U);
    EXPECT_EQ(spans[0].kind, SemanticKind::GlobalVariable);

    std::vector<frost::build::BuildDiagnostic> build(2);
    build[0].diagnostic.severity = frost::build::Severity::Warning;
    build[0].diagnostic.code = "C4189";
    build[0].diagnostic.message = "'x': local variable is initialized but not referenced";
    build[0].diagnostic.line = 3;
    build[0].diagnostic.column = 9;
    build[1].diagnostic.message = "a tool error without a line";
    const auto fromBuild = frost::ui::lang::toEditorDiagnostics(std::span<const frost::build::BuildDiagnostic>(build));
    ASSERT_EQ(fromBuild.size(), 1U);
    EXPECT_EQ(fromBuild[0].severity, InsightSeverity::Warning);
    EXPECT_EQ(fromBuild[0].message, "C4189: 'x': local variable is initialized but not referenced");
    EXPECT_EQ(fromBuild[0].line, 3);
    EXPECT_FALSE(fromBuild[0].range.has_value());
}

TEST(EditorInsightBinding, SemanticTokensDiagnosticsAndInactiveCodeReachTheView) {
    FROST_REQUIRE_OFFSCREEN();
    FeatureServer server(capabilities());
    const std::string path = workPath("main.cpp");
    constexpr std::string_view text = "int f(int factor) {\n"
                                      "  int local = factor;\n"
                                      "#if 0\n"
                                      "  int disabled;\n"
                                      "#endif\n"
                                      "}\n";
    // factor (parameter, declaration) at 0:10; local (variable, functionScope) at 1:6; factor at
    // 1:14; the disabled line (comment) at 3:0. The second answer has every column of line 0
    // three further on (the edit below types three characters there).
    const std::vector<int> first{0, 10, 6, 1, 1, 1, 6, 5, 0, 3, 0, 8, 6, 1, 0, 2, 0, 15, 2, 0};
    const std::vector<int> second{0, 13, 6, 1, 1, 1, 6, 5, 0, 3, 0, 8, 6, 1, 0, 2, 0, 15, 2, 0};
    server.on("textDocument/semanticTokens/full",
              [&](const Json&) { return Json::object().set("resultId", "1").set("data", integers(first)); });
    server.on("textDocument/semanticTokens/full/delta",
              [&](const Json&) { return Json::object().set("resultId", "2").set("data", integers(second)); });

    frost::ui::test::EditorHarness h(text, at(path));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()}); // The view owns it.
    binding->setDebounce(std::chrono::milliseconds(10));
    EXPECT_TRUE(h.view->insightEnabled());
    binding->refreshSemanticTokens();
    ASSERT_TRUE(pumpUntil([&] { return !h.view->semanticTokens().empty(); }));
    const std::vector<frost::ui::SemanticSpan>& spans = h.view->semanticTokens();
    ASSERT_EQ(spans.size(), 3U);
    EXPECT_EQ(spans[0].range, find(text, "factor"));
    EXPECT_EQ(spans[0].kind, SemanticKind::Parameter);
    EXPECT_EQ(spans[1].range, find(text, "local"));
    EXPECT_EQ(spans[1].kind, SemanticKind::LocalVariable);
    // The comment token stands in for the inactive region.
    ASSERT_EQ(h.view->inactiveRanges().size(), 1U);
    EXPECT_EQ(h.view->editor().text().text(h.view->inactiveRanges()[0]), "  int disabled;");
    ASSERT_TRUE(binding->lastTokens().has_value());
    EXPECT_EQ(binding->lastTokens()->resultId, "1");

    // Live diagnostics of the current text: no longer analysing.
    fl::FileDiagnostics live;
    live.file = path;
    live.textVersion = h.view->editor().text().version();
    fl::LiveDiagnostic unused;
    unused.severity = fl::DiagnosticSeverity::Warning;
    unused.message = "unused variable 'local'";
    unused.range = find(text, "local");
    unused.textVersion = live.textVersion;
    live.diagnostics.push_back(unused);
    binding->setLiveDiagnostics(live);
    ASSERT_EQ(h.view->diagnostics().size(), 1U);
    EXPECT_EQ(h.view->diagnostics()[0].severity, InsightSeverity::Warning);
    EXPECT_FALSE(h.view->insightSummary().analyzing);
    // Another file's diagnostics are ignored.
    fl::FileDiagnostics other = live;
    other.file = workPath("other.cpp");
    binding->setLiveDiagnostics(other);
    EXPECT_EQ(h.view->diagnostics().size(), 1U);

    // An edit: the old tokens move with the text, the view analyses, and the debounced request is
    // a delta naming the previous result.
    h.view->setState(frost::text::EditorState::caretAt(ByteOffset{6}));
    ASSERT_TRUE(h.view->typeText("abc"));
    EXPECT_TRUE(h.view->insightSummary().analyzing);
    EXPECT_EQ(h.view->semanticTokens()[0].range.begin, ByteOffset{13});
    ASSERT_TRUE(pumpUntil([&] { return binding->lastTokens() && binding->lastTokens()->resultId == "2"; }));
    const std::vector<Json> deltas = server.paramsOf("textDocument/semanticTokens/full/delta");
    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_EQ(deltas[0].stringAt("previousResultId"), "1");
    EXPECT_EQ(h.view->semanticTokens()[0].range.begin, ByteOffset{13});
    EXPECT_EQ(h.view->semanticTokens()[0].kind, SemanticKind::Parameter);

    // The extension's notification replaces the comment-token stand-in for good.
    fl::InactiveRegions regions;
    regions.file = path;
    regions.ranges = {find(h.view->editor().text().text(), "#endif")};
    regions.textVersion = h.view->editor().text().version();
    binding->setInactiveRegions(regions);
    EXPECT_EQ(h.view->inactiveRanges(), regions.ranges);

    // Build diagnostics join the live ones.
    std::vector<frost::build::BuildDiagnostic> build(1);
    build[0].diagnostic.severity = frost::build::Severity::Error;
    build[0].diagnostic.code = "C2065";
    build[0].diagnostic.message = "'missing': undeclared identifier";
    build[0].diagnostic.line = 6;
    build[0].diagnostic.column = 1;
    build[0].path = path;
    binding->setBuildDiagnostics(build);
    EXPECT_EQ(h.view->insightSummary().count(InsightSeverity::Error), 1);
    EXPECT_EQ(h.view->insightSummary().count(InsightSeverity::Warning), 1);
    binding->setServerAvailable(false);
    EXPECT_FALSE(h.view->insightSummary().analyzing);
}

TEST(EditorInsightTranslation, ImplementationLensOpensSubtypeResults) {
    const std::vector<frost::ui::lang::CodeVisionCountEntry> entries{
        {ByteOffset{6}, frost::LineIndex{0}, {std::nullopt, std::nullopt, 2}},
    };
    const auto lenses = frost::ui::lang::toCodeVisionCountLenses(entries);
    ASSERT_EQ(lenses.size(), 1U);
    EXPECT_EQ(lenses.front().label, "2 implementations");
    EXPECT_EQ(lenses.front().actionId, "navigate.typeImplementations");
}

TEST(EditorInsightBinding, CSharpCodeVisionRetainsGitAuthorsWithoutCppIndexRequests) {
    FROST_REQUIRE_OFFSCREEN();
    FeatureServer server(capabilities().set("documentSymbolProvider", true));
    server.on("textDocument/documentSymbol", [](const Json&) {
        const Json range = Json::object()
                               .set("start", Json::object().set("line", 0).set("character", 6))
                               .set("end", Json::object().set("line", 0).set("character", 7));
        return Json::array(
            {Json::object().set("name", "C").set("kind", 5).set("range", range).set("selectionRange", range)});
    });
    int cppRequests = 0;
    const auto rejectCppRequest = [&](const Json&) {
        ++cppRequests;
        return Json::array();
    };
    server.on("textDocument/references", rejectCppRequest);
    server.on("textDocument/implementation", rejectCppRequest);
    server.on("textDocument/prepareTypeHierarchy", rejectCppRequest);
    frost::ui::test::EditorHarness h("class C {}\n", at(workPath("authors.cs")));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()});
    binding->setCodeVisionEnabled(true);
    binding->setAuthorProvider([](int line) { return line == 1 ? std::string("Alice") : std::string(); });
    binding->refreshCodeVision();
    ASSERT_TRUE(pumpUntil([&] { return !h.view->codeVisionLenses().empty(); }));
    ASSERT_EQ(h.view->codeVisionLenses().size(), 1U);
    EXPECT_EQ(h.view->codeVisionLenses().front().label, "Alice");
    EXPECT_EQ(h.view->codeVisionLenses().front().actionId, "vcs.annotate");
    EXPECT_EQ(cppRequests, 0);
}

TEST(EditorInsightBinding, CodeVisionIncludesMethodsInsideNamespacedClasses) {
    FROST_REQUIRE_OFFSCREEN();
    FeatureServer server(capabilities().set("documentSymbolProvider", true));
    const std::string path = workPath("namespaced.cpp");
    constexpr std::string_view text = "namespace N {\nclass C {\nvoid method();\n};\n}\n";
    server.on("textDocument/documentSymbol", [](const Json&) {
        const auto symbol = [](std::string name, int kind, int line, int column) {
            const Json range = Json::object()
                                   .set("start", Json::object().set("line", line).set("character", column))
                                   .set("end", Json::object().set("line", line).set("character", column + 1));
            return Json::object()
                .set("name", std::move(name))
                .set("kind", kind)
                .set("range", range)
                .set("selectionRange", range);
        };
        Json klass = symbol("C", 5, 1, 6);
        klass.set("children", Json::array({symbol("method", 6, 2, 5)}));
        Json space = symbol("N", 3, 0, 10);
        space.set("children", Json::array({std::move(klass)}));
        return Json::array({std::move(space)});
    });
    server.on("textDocument/references", [](const Json&) { return Json::array(); });
    server.on("textDocument/implementation", [](const Json&) { return Json::array(); });
    server.on("textDocument/prepareTypeHierarchy", [](const Json&) { return Json::array(); });
    frost::ui::test::EditorHarness h(text, at(path));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()});
    binding->setCodeVisionEnabled(true);
    binding->refreshCodeVision();
    ASSERT_TRUE(pumpUntil([&] {
        return std::ranges::any_of(h.view->codeVisionLenses(), [](const frost::ui::EditorCodeVisionLens& lens) {
            return lens.line == frost::LineIndex{2} && lens.label == "0 overrides";
        });
    }));
}

TEST(EditorInsightBinding, InlayHintsCodeVisionAndInlineDebugValuesRenderInDisplayList) {
    FROST_REQUIRE_OFFSCREEN();
    Json caps = capabilities().set("inlayHintProvider", true).set("documentSymbolProvider", true);
    FeatureServer server(std::move(caps));
    const std::string path = workPath("hints.cpp");
    constexpr std::string_view text = "class Widget {\n"
                                      "  int compute(int count) {\n"
                                      "    auto value = compute(42);\n"
                                      "    return value;\n"
                                      "  }\n"
                                      "};\n";
    server.on("textDocument/semanticTokens/full",
              [&](const Json&) { return Json::object().set("resultId", "1").set("data", Json::array()); });
    server.on("textDocument/inlayHint", [&](const Json&) {
        return Json::array({
            Json::object()
                .set("position", Json::object().set("line", 2).set("character", 8))
                .set("label", ": int")
                .set("kind", 1)
                .set("tooltip", "Deduced type int"),
            Json::object()
                .set("position", Json::object().set("line", 2).set("character", 25))
                .set("label", "count:")
                .set("kind", 2)
                .set("tooltip", "Parameter count"),
        });
    });
    server.on("textDocument/documentSymbol", [&](const Json&) {
        return Json::array({
            Json::object()
                .set("name", "Widget")
                .set("kind", 5)
                .set("range", Json::object()
                                  .set("start", Json::object().set("line", 0).set("character", 0))
                                  .set("end", Json::object().set("line", 5).set("character", 2)))
                .set("selectionRange", Json::object()
                                           .set("start", Json::object().set("line", 0).set("character", 6))
                                           .set("end", Json::object().set("line", 0).set("character", 12)))
                .set(
                    "children",
                    Json::array({
                        Json::object()
                            .set("name", "compute")
                            .set("kind", 6)
                            .set("range", Json::object()
                                              .set("start", Json::object().set("line", 1).set("character", 2))
                                              .set("end", Json::object().set("line", 4).set("character", 3)))
                            .set("selectionRange", Json::object()
                                                       .set("start", Json::object().set("line", 1).set("character", 6))
                                                       .set("end", Json::object().set("line", 1).set("character", 13))),
                    })),
        });
    });
    server.on("textDocument/references", [&](const Json&) { return Json::array(); });
    server.on("textDocument/implementation", [&](const Json&) { return Json::array(); });
    server.on("textDocument/prepareTypeHierarchy", [&](const Json&) { return Json::array(); });

    frost::ui::test::EditorHarness h(text, at(path));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()});
    binding->setCodeVisionEnabled(true);
    binding->setAuthorProvider([](int line) { return line == 1 ? std::string("Alice") : std::string(); });
    binding->refreshSemanticTokens();

    ASSERT_TRUE(pumpUntil([&] { return h.view->inlayHints().size() == 2U && !h.view->codeVisionLenses().empty(); }));

    h.view->setInlineDebuggerValues({
        frost::ui::EditorInlineValue{frost::LineIndex{2}, "value = 42", "Debugger value"},
    });
    h.view->setDebugHoverProvider([](std::string_view expr, frost::LineIndex) {
        return expr == "value" ? QStringLiteral("value (int) = 42") : QString();
    });

    const frost::ui::EditorDisplayList list = h.view->buildDisplayList();
    EXPECT_NE(list.findText(QStringLiteral(": int")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("count:")), nullptr);
    // Zero is shown only because the mock index answered with an empty reference set.
    EXPECT_NE(list.findText(QStringLiteral("0 usages")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("0 overrides")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("Alice")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("value = 42")), nullptr);

    const auto checkInlayGeometry = [&](const frost::ui::EditorDisplayList& display) {
        const frost::ui::DisplayText* typeHint = display.findText(QStringLiteral(": int"));
        const auto autoRange = find(text, "auto");
        const auto valueRange = find(text, "value");
        // Plain source runs may group whitespace and several tokens. Locate source bytes, then
        // measure the prefix within that run rather than assuming a syntax-token boundary.
        const frost::ui::DisplayText* before = display.findTextAt(autoRange.begin);
        const frost::ui::DisplayText* after = display.findTextAt(valueRange.begin);
        ASSERT_NE(typeHint, nullptr);
        ASSERT_NE(before, nullptr);
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(typeHint->source.begin, autoRange.end);
        EXPECT_TRUE(typeHint->source.empty());
        EXPECT_EQ(before->source.end, autoRange.end);
        EXPECT_EQ(after->source.begin, autoRange.end);
        const QFontMetricsF metrics(display.font);
        const auto prefixWidth = [&](ByteOffset begin, ByteOffset end) {
            return metrics.horizontalAdvance(QString::fromStdString(h.view->editor().text().text({begin, end})));
        };
        EXPECT_NEAR(typeHint->origin.x(), before->origin.x() + prefixWidth(before->source.begin, autoRange.end), 1.0);
        const double valueX = after->origin.x() + prefixWidth(after->source.begin, valueRange.begin);
        EXPECT_NEAR(valueX,
                    typeHint->origin.x() + metrics.horizontalAdvance(QStringLiteral(": int")) +
                        prefixWidth(autoRange.end, valueRange.begin),
                    1.0);
    };
    checkInlayGeometry(list);

    h.view->reloadFromDocument();
    const frost::ui::EditorDisplayList reloaded = h.view->buildDisplayList();
    EXPECT_NE(reloaded.findText(QStringLiteral(": int")), nullptr);
    EXPECT_NE(reloaded.findText(QStringLiteral("count:")), nullptr);
    checkInlayGeometry(reloaded);
}

TEST(EditorInsightBinding, LuaCommentTokensDoNotBecomeInactivePreprocessorRanges) {
    FROST_REQUIRE_OFFSCREEN();
    FeatureServer server(capabilities());
    const std::string path = workPath("main.lua");
    constexpr std::string_view text = "---Doc comment\nlocal value = 1\n";
    // Line 0 comment token (0..14), line 1 variable token (6..11).
    const std::vector<int> data{0, 0, 14, 2, 0, 1, 6, 5, 0, 2};
    server.on("textDocument/semanticTokens/full",
              [&](const Json&) { return Json::object().set("resultId", "1").set("data", integers(data)); });

    frost::ui::test::EditorHarness h(text, at(path));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()});
    binding->refreshSemanticTokens();
    ASSERT_TRUE(pumpUntil([&] { return !h.view->semanticTokens().empty(); }));
    EXPECT_EQ(h.view->semanticTokens().size(), 1U);
    EXPECT_TRUE(h.view->inactiveRanges().empty());
}

/*
 * CPP-5 over the scripted server: document links and folding ranges are requested with the tokens
 * when the server offers them; the link reaches the view as a byte range with its file target
 * (Ctrl+click on it emits documentLinkActivated, not ctrlClicked), and a folding range becomes a
 * fold candidate beside the syntax tree's, until the next edit.
 */
TEST(EditorInsightBinding, DocumentLinksAndFoldingRangesReachTheView) {
    FROST_REQUIRE_OFFSCREEN();
    Json caps = capabilities()
                    .set("documentLinkProvider", Json::object().set("resolveProvider", false))
                    .set("foldingRangeProvider", true);
    FeatureServer server(std::move(caps));
    const std::string path = workPath("links.cpp");
    const std::string header = workPath("links.h");
    constexpr std::string_view text = "#include \"links.h\"\n"
                                      "/* one\n"
                                      "   two\n"
                                      "   three */\n"
                                      "int f() {\n"
                                      "  return 1;\n"
                                      "}\n";
    server.on("textDocument/semanticTokens/full",
              [&](const Json&) { return Json::object().set("resultId", "1").set("data", Json::array()); });
    const auto position = [](int line, int character) {
        return Json::object().set("line", line).set("character", character);
    };
    server.on("textDocument/documentLink", [&](const Json&) {
        return Json::array({Json::object()
                                .set("range", Json::object().set("start", position(0, 9)).set("end", position(0, 18)))
                                .set("target", fl::pathToUri(header).value())});
    });
    // The comment (lines 1-3) and f's body (lines 4-6), as clangd answers them.
    server.on("textDocument/foldingRange", [&](const Json&) {
        return Json::array(
            {Json::object().set("startLine", 1).set("endLine", 3).set("kind", "comment"), Json::object()
                                                                                              .set("startLine", 4)
                                                                                              .set("startCharacter", 9)
                                                                                              .set("endLine", 6)
                                                                                              .set("endCharacter", 0)
                                                                                              .set("kind", "region")});
    });

    frost::ui::test::EditorHarness h(text, at(path));
    auto* binding = new EditorInsightBinding(*h.view, {server.client(), server.scheduler()});
    bool linksApplied = false;
    bool foldsApplied = false;
    QObject::connect(binding, &EditorInsightBinding::documentLinksApplied, [&] { linksApplied = true; });
    QObject::connect(binding, &EditorInsightBinding::foldingRangesApplied, [&] { foldsApplied = true; });
    binding->refreshSemanticTokens();
    ASSERT_TRUE(pumpUntil([&] { return linksApplied && foldsApplied; }));

    ASSERT_EQ(h.view->documentLinks().size(), 1U);
    const frost::ui::EditorDocumentLink link = h.view->documentLinks().front();
    EXPECT_EQ(link.range, find(text, "\"links.h\""));
    EXPECT_EQ(fl::documentKey(link.target), fl::documentKey(header));
    EXPECT_EQ(h.view->documentLinkAt(link.range.begin), link);
    EXPECT_FALSE(h.view->documentLinkAt(link.range.end).has_value());

    // Each folding range spans from the end of its first line to the start of its last.
    const frost::text::Snapshot snapshot = h.view->editor().text();
    const auto lineEnd = [&](std::uint32_t line) { return snapshot.lineInfo(frost::LineIndex{line})->content.end; };
    const auto lineStart = [&](std::uint32_t line) { return snapshot.lineInfo(frost::LineIndex{line})->content.begin; };
    const std::vector<frost::text::ByteRange> expected{{lineEnd(1), lineStart(3)}, {lineEnd(4), lineStart(6)}};
    EXPECT_EQ(h.view->languageFoldRanges(), expected);
    for (const frost::text::ByteRange& range : expected) {
        EXPECT_NE(std::ranges::find(h.view->foldCandidates(), range), h.view->foldCandidates().end());
    }

    // Ctrl+click on the link opens it; the ordinary navigation click is not sent.
    QStringList opened;
    int ctrlClicks = 0;
    QObject::connect(h.view.get(), &frost::ui::EditorView::documentLinkActivated,
                     [&](const QString& target) { opened.push_back(target); });
    QObject::connect(h.view.get(), &frost::ui::EditorView::ctrlClicked, [&](frost::ByteOffset) { ++ctrlClicks; });
    const auto clickAt = [&](frost::ByteOffset offset) {
        const QRect caret = h.view->globalCaretRect(offset);
        const QPoint local = h.view->viewport()->mapFromGlobal(caret.center() + QPoint(2, 0));
        const QPointF global(h.view->viewport()->mapToGlobal(local));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), global, Qt::LeftButton, Qt::LeftButton,
                          Qt::ControlModifier);
        QCoreApplication::sendEvent(h.view->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), global, Qt::LeftButton, Qt::NoButton,
                            Qt::ControlModifier);
        QCoreApplication::sendEvent(h.view->viewport(), &release);
    };
    clickAt(ByteOffset{link.range.begin.value() + 3});
    ASSERT_EQ(opened.size(), 1);
    EXPECT_EQ(fl::documentKey(opened.front().toStdString()), fl::documentKey(header));
    EXPECT_EQ(ctrlClicks, 0);
    clickAt(find(text, "return").begin);
    EXPECT_EQ(opened.size(), 1);
    EXPECT_EQ(ctrlClicks, 1);

    // An edit keeps the link on its text and refuses folds computed for the old text.
    h.view->setState(frost::text::EditorState::caretAt(ByteOffset{0}));
    ASSERT_TRUE(h.view->typeText("// x\n"));
    ASSERT_EQ(h.view->documentLinks().size(), 1U);
    EXPECT_EQ(h.view->editor().text().text(h.view->documentLinks().front().range), "\"links.h\"");
    EXPECT_FALSE(h.view->setLanguageFoldRanges(snapshot.version(), expected)) << "folds of an older text";
}

} // namespace
