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
#include <QThread>
#include <gtest/gtest.h>

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
    // FND-3 (D32): no usage count guessed from the text and no fixed "implementations" label
    // (counts arrive with CPP-9); the Git author lens is real data and stays.
    EXPECT_EQ(list.findText(QStringLiteral("1 usage")), nullptr);
    EXPECT_EQ(list.findText(QStringLiteral("usages")), nullptr);
    EXPECT_EQ(list.findText(QStringLiteral("implementations")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("Alice")), nullptr);
    EXPECT_NE(list.findText(QStringLiteral("value = 42")), nullptr);
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

} // namespace
