// SPDX-License-Identifier: Apache-2.0
//
// EditorView insight tests (offscreen, ADR 0047): semantic tokens layered over the tree-sitter
// spans in the theme's Semantic.* colours and kept on their text through edits, inactive regions
// dimmed, diagnostics as squiggles with tooltips (wavy for errors and warnings, dotted for weak
// warnings) and owner decorations, the error stripe's marks at their relative positions with
// click-to-jump, the inspection widget's counts, arrows and states, the status-bar indicator, and
// F2 / Shift+F2 with the balloon. Everything is checked in the display list, not as pixels.

#include "frost/ui/editor/editor_colors.hpp"
#include "frost/ui/editor/insight_status.hpp"

#include "editor/editor_harness.hpp"
#include "support/appearance_fixture.hpp"
#include <QImage>
#include <QMouseEvent>
#include <QScrollBar>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using test::EditorHarness;
using test::HarnessOptions;

constexpr std::string_view kSample = "int scaled(int factor) {\n"
                                     "    int local = factor * 2;\n"
                                     "    return local;\n"
                                     "}\n";

HarnessOptions cpp(AppearanceManager* appearance = nullptr) {
    HarnessOptions options;
    options.path = "sample.cpp";
    options.syntax = true;
    options.appearance = appearance;
    return options;
}

// The byte range of the `nth` (0-based) occurrence of `word` in `text`.
text::ByteRange find(std::string_view text, std::string_view word, std::size_t nth = 0) {
    std::size_t at = text.find(word);
    for (std::size_t i = 0; i < nth && at != std::string_view::npos; ++i) {
        at = text.find(word, at + 1);
    }
    EXPECT_NE(at, std::string_view::npos) << word;
    return text::ByteRange{ByteOffset{at}, ByteOffset{at + word.size()}};
}

std::uint64_t version(const EditorHarness& h) { return h.view->editor().text().version(); }

EditorDiagnostic problem(InsightSeverity severity, text::ByteRange range, std::string message) {
    EditorDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.range = range;
    diagnostic.message = std::move(message);
    return diagnostic;
}

void click(EditorView& view, QPointF at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, view.viewport()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(view.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view.viewport()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(view.viewport(), &release);
}

// A document of `lines` short lines ("line 0" ...).
std::string manyLines(int lines) {
    std::string out;
    for (int i = 0; i < lines; ++i) {
        out += "line " + std::to_string(i) + "\n";
    }
    return out;
}

TEST(EditorInsight, SemanticTokensWinOverTreeSitterInTheThemeColours) {
    FROST_REQUIRE_OFFSCREEN();
    AppearanceSettings appearance;
    appearance.theme = "glacier-dark";
    const auto installed = test::install(appearance);
    ASSERT_TRUE(installed);
    EditorHarness h(kSample, cpp(installed->manager.get()));
    const Theme& theme = *installed->manager->themeSnapshot();

    const text::ByteRange param = find(kSample, "factor", 1);
    const text::ByteRange local = find(kSample, "local", 0);
    ASSERT_TRUE(h.view->setSemanticTokens(
        version(h), {SemanticSpan{local, SemanticKind::LocalVariable, semantic_modifiers::kDeclaration},
                     SemanticSpan{param, SemanticKind::Parameter, 0}}));
    const EditorDisplayList list = h.view->buildDisplayList();
    const DisplayText* paramRun = list.findTextAt(param.begin);
    ASSERT_NE(paramRun, nullptr);
    EXPECT_EQ(paramRun->text, QStringLiteral("factor"));
    EXPECT_EQ(paramRun->semantic, SemanticKind::Parameter);
    EXPECT_EQ(paramRun->color, toQColor(theme.color(ColorKey::SemanticParameter)));
    EXPECT_NE(paramRun->color, toQColor(theme.color(ColorKey::EditorForeground)));
    const DisplayText* localRun = list.findTextAt(local.begin);
    ASSERT_NE(localRun, nullptr);
    EXPECT_EQ(localRun->semantic, SemanticKind::LocalVariable);
    EXPECT_EQ(localRun->color, toQColor(theme.color(ColorKey::SemanticLocalVariable)));
    // Tree-sitter keeps everything without a token.
    const DisplayText* keyword = list.findText(QStringLiteral("return"));
    ASSERT_NE(keyword, nullptr);
    EXPECT_FALSE(keyword->semantic.has_value());
    EXPECT_EQ(keyword->color, toQColor(theme.color(ColorKey::SyntaxKeyword)));
    EXPECT_EQ(colorKeyFor(SemanticKind::Macro), ColorKey::SemanticMacro);

    // Typing before the tokens moves them with their text until fresh ones arrive.
    const std::uint64_t before = version(h);
    h.view->setState(text::EditorState::caretAt(ByteOffset{0}));
    ASSERT_TRUE(h.view->typeText("xx "));
    h.settle();
    const EditorDisplayList edited = h.view->buildDisplayList();
    const DisplayText* moved = edited.findTextAt(param.begin + 3);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->text, QStringLiteral("factor"));
    EXPECT_EQ(moved->semantic, SemanticKind::Parameter);
    // Tokens computed for the old text are mapped through the edit.
    ASSERT_TRUE(h.view->setSemanticTokens(before, {SemanticSpan{local, SemanticKind::LocalVariable, 0}}));
    ASSERT_EQ(h.view->semanticTokens().size(), 1U);
    EXPECT_EQ(h.view->semanticTokens()[0].range, (text::ByteRange{local.begin + 3, local.end + 3}));
    // A version the view never had is refused.
    EXPECT_FALSE(h.view->setSemanticTokens(version(h) + 10, {}));
    EXPECT_EQ(h.view->semanticTokens().size(), 1U);
}

TEST(EditorInsight, ReadonlyStaticAndDeprecatedModifiers) {
    FROST_REQUIRE_OFFSCREEN();
    constexpr std::string_view text = "int kLimit = old();\n";
    EditorHarness h(text);
    const text::ByteRange limit = find(text, "kLimit");
    const text::ByteRange old = find(text, "old");
    ASSERT_TRUE(h.view->setSemanticTokens(
        version(h), {SemanticSpan{limit, SemanticKind::GlobalVariable, semantic_modifiers::kReadonly},
                     SemanticSpan{old, SemanticKind::Function, semantic_modifiers::kDeprecated}}));
    const EditorDisplayList list = h.view->buildDisplayList();
    EXPECT_EQ(list.findTextAt(limit.begin)->color, h.view->colors().semanticConstant);
    const auto strikes = list.decorationsWithStyle(DecorationStyle::Strikeout);
    ASSERT_EQ(strikes.size(), 1U);
    EXPECT_EQ(strikes[0]->source, old);
    EXPECT_TRUE(h.view->colors().semanticFormat(SemanticKind::StaticMethod, 0).italic);
}

TEST(EditorInsight, InactiveRegionsAreDimmedOverSemanticColours) {
    FROST_REQUIRE_OFFSCREEN();
    constexpr std::string_view text = "#ifdef FROST_NOT_DEFINED\nint disabledCode;\n#endif\nint live;\n";
    EditorHarness h(text, cpp());
    const text::ByteRange disabled = find(text, "int disabledCode;");
    const text::ByteRange live = find(text, "live");
    ASSERT_TRUE(h.view->setSemanticTokens(version(h),
                                          {SemanticSpan{find(text, "disabledCode"), SemanticKind::GlobalVariable, 0},
                                           SemanticSpan{live, SemanticKind::GlobalVariable, 0}}));
    ASSERT_TRUE(h.view->setInactiveRanges(version(h), {disabled}));
    const EditorDisplayList list = h.view->buildDisplayList();
    const DisplayText* dimmed = list.findTextAt(find(text, "disabledCode").begin);
    ASSERT_NE(dimmed, nullptr);
    EXPECT_TRUE(dimmed->inactive);
    EXPECT_EQ(dimmed->color, h.view->colors().inactiveCode);
    const DisplayText* active = list.findTextAt(live.begin);
    ASSERT_NE(active, nullptr);
    EXPECT_FALSE(active->inactive);
    EXPECT_EQ(active->color, h.view->colors().semanticFormat(SemanticKind::GlobalVariable, 0).color);
    EXPECT_EQ(h.view->inactiveRanges(), std::vector<text::ByteRange>{disabled});
}

TEST(EditorInsight, DiagnosticsDrawSquigglesWithTooltips) {
    FROST_REQUIRE_OFFSCREEN();
    constexpr std::string_view text = "int a = missing;\nint unused = 1;\nint b = a;\nint c = bar;\n";
    EditorHarness h(text);
    const text::ByteRange missing = find(text, "missing");
    const text::ByteRange unused = find(text, "unused");
    const text::ByteRange weak = find(text, "b");
    h.view->setDiagnostics(EditorDiagnosticSource::Live,
                           {problem(InsightSeverity::Error, missing, "use of undeclared identifier 'missing'"),
                            problem(InsightSeverity::Warning, unused, "unused variable 'unused'"),
                            problem(InsightSeverity::WeakWarning, weak, "could be const")});
    // A build error by line and column (the identifier there), and a duplicate of a live one.
    EditorDiagnostic build;
    build.severity = InsightSeverity::Error;
    build.message = "C2065: 'bar': undeclared identifier";
    build.line = 4;
    build.column = 9;
    EditorDiagnostic duplicate;
    duplicate.severity = InsightSeverity::Error;
    duplicate.message = "use of undeclared identifier 'missing'";
    duplicate.line = 1;
    duplicate.column = 9;
    h.view->setDiagnostics(EditorDiagnosticSource::Build, {build, duplicate});
    const std::vector<ActiveDiagnostic> all = h.view->diagnostics();
    ASSERT_EQ(all.size(), 4U);
    EXPECT_EQ(all.back().range, find(text, "bar"));
    EXPECT_EQ(all.back().source, EditorDiagnosticSource::Build);

    const EditorDisplayList list = h.view->buildDisplayList();
    const auto wavy = list.decorationsWithStyle(DecorationStyle::WavyUnderline);
    ASSERT_EQ(wavy.size(), 3U); // Two errors and the warning.
    const auto dotted = list.decorationsWithStyle(DecorationStyle::DottedUnderline);
    ASSERT_EQ(dotted.size(), 1U);
    EXPECT_EQ(dotted[0]->source, weak);
    EXPECT_EQ(dotted[0]->severity, InsightSeverity::WeakWarning);
    const auto error = std::ranges::find_if(wavy, [&](const DisplayDecoration* d) { return d->source == missing; });
    ASSERT_NE(error, wavy.end());
    EXPECT_EQ((*error)->color, h.view->colors().severityColor(InsightSeverity::Error));
    EXPECT_EQ((*error)->tooltip, QStringLiteral("use of undeclared identifier 'missing'"));
    const DisplayText* run = list.findTextAt(missing.begin);
    ASSERT_NE(run, nullptr);
    EXPECT_GT((*error)->rect.width(), 0.0);
    EXPECT_GT((*error)->baseline, (*error)->rect.top());
    // Hovering the squiggle shows the message.
    EXPECT_EQ(h.view->toolTipAt((*error)->rect.center()), QStringLiteral("use of undeclared identifier 'missing'"));

    // Another owner's decorations join the list, tooltips included.
    h.view->setDecorations("spelling", {EditorDecoration{find(text, "unused"), DecorationStyle::Underline, std::nullopt,
                                                         QColor(Qt::green), "Typo: unused"}});
    const EditorDisplayList decorated = h.view->buildDisplayList();
    const auto underlines = decorated.decorationsWithStyle(DecorationStyle::Underline);
    ASSERT_EQ(underlines.size(), 1U);
    EXPECT_EQ(underlines[0]->color, QColor(Qt::green));
    EXPECT_TRUE(h.view->toolTipAt(underlines[0]->rect.center()).contains(QStringLiteral("Typo: unused")));
    h.view->setDecorations("spelling", {});
    EXPECT_TRUE(h.view->decorations().empty());
    // Painting the frame through the raster backend draws every part of it.
    EXPECT_FALSE(h.view->viewport()->grab().toImage().isNull());
}

TEST(EditorInsight, StripeMarksSitAtRelativePositionsAndClickJumps) {
    FROST_REQUIRE_OFFSCREEN();
    const std::string text = manyLines(200);
    EditorHarness h(text);
    const text::ByteRange error = find(text, "line 49");
    const text::ByteRange warning = find(text, "line 149");
    h.view->setDiagnostics(EditorDiagnosticSource::Live,
                           {problem(InsightSeverity::Error, error, "an error at 50"),
                            problem(InsightSeverity::Warning, warning, "a warning at 150")});
    h.view->setUsageHighlights({UsageHighlight{find(text, "line 99"), true}});
    const EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_FALSE(list.stripe.isEmpty());
    EXPECT_DOUBLE_EQ(list.stripe.right(), static_cast<double>(h.view->viewport()->width()));
    EXPECT_LE(list.textArea.right(), list.stripe.left());
    EXPECT_EQ(list.stripeSummaryColor, h.view->colors().severityColor(InsightSeverity::Error));
    const auto markOf = [&](StripeMarkKind kind) -> const DisplayStripeMark* {
        const auto it = std::ranges::find(list.stripeMarks, kind, &DisplayStripeMark::kind);
        return it != list.stripeMarks.end() ? &*it : nullptr;
    };
    const DisplayStripeMark* errorMark = markOf(StripeMarkKind::Error);
    const DisplayStripeMark* warningMark = markOf(StripeMarkKind::Warning);
    const DisplayStripeMark* usageMark = markOf(StripeMarkKind::WriteUsage);
    ASSERT_NE(errorMark, nullptr);
    ASSERT_NE(warningMark, nullptr);
    ASSERT_NE(usageMark, nullptr);
    EXPECT_EQ(errorMark->line, LineIndex{49});
    EXPECT_EQ(errorMark->color, h.view->colors().severityColor(InsightSeverity::Error));
    // Scaled to the document: the marks sit at their lines' share of the stripe below the summary.
    const double top = list.stripeSummary.bottom();
    const double span = list.stripe.bottom() - top;
    EXPECT_NEAR((errorMark->rect.top() - top) / span, 49.0 / 201.0, 0.03);
    EXPECT_NEAR((usageMark->rect.top() - top) / span, 99.0 / 201.0, 0.03);
    EXPECT_NEAR((warningMark->rect.top() - top) / span, 149.0 / 201.0, 0.03);
    EXPECT_LT(usageMark->rect.width(), errorMark->rect.width()); // Usages take the left half.
    EXPECT_EQ(h.view->toolTipAt(warningMark->rect.center()), QStringLiteral("a warning at 150"));

    // A click on a mark jumps there and scrolls it into view.
    EXPECT_EQ(h.view->verticalScrollBar()->value(), 0);
    click(*h.view, warningMark->rect.center());
    EXPECT_EQ(h.caret(), warning.begin.value());
    EXPECT_GT(h.view->verticalScrollBar()->value(), 100);
    EXPECT_EQ(h.view->problemBalloonText(), QStringLiteral("a warning at 150"));
    EXPECT_TRUE(h.view->clickInsight(errorMark->rect.center()));
    EXPECT_EQ(h.caret(), error.begin.value());
    // A click on the summary box goes to the next problem; elsewhere in the text it is a normal click.
    EXPECT_TRUE(h.view->clickInsight(list.stripeSummary.center()));
    EXPECT_EQ(h.caret(), error.begin.value()); // The only error (errors before warnings).
    EXPECT_FALSE(h.view->clickInsight(QPointF(list.textArea.center().x(), 5.0)));
}

TEST(EditorInsight, InspectionWidgetCountsArrowsAndStates) {
    FROST_REQUIRE_OFFSCREEN();
    constexpr std::string_view text = "int a = x;\nint b = y;\nint c;\n";
    EditorHarness h(text);
    int changed = 0;
    QObject::connect(h.view.get(), &EditorView::insightChanged, [&changed] { ++changed; });
    EXPECT_EQ(h.view->buildDisplayList().inspection.state, InspectionState::Hidden);

    h.view->setInsightEnabled(true);
    EditorDisplayList list = h.view->buildDisplayList();
    EXPECT_EQ(list.inspection.state, InspectionState::Clean);
    EXPECT_TRUE(std::ranges::any_of(list.chrome, [&](const DisplayShape& shape) {
        return shape.kind == DisplayShape::Kind::Polyline && shape.stroke == h.view->colors().cleanMarker;
    }));
    EXPECT_EQ(list.stripeSummaryColor, h.view->colors().cleanMarker);

    h.view->setAnalyzing(true);
    list = h.view->buildDisplayList();
    EXPECT_EQ(list.inspection.state, InspectionState::Analyzing);
    EXPECT_EQ(std::ranges::count(list.chrome, DisplayShape::Kind::Ellipse, &DisplayShape::kind), 8); // The spinner.
    h.view->setAnalyzing(false);

    h.view->setDiagnostics(EditorDiagnosticSource::Live,
                           {problem(InsightSeverity::Error, find(text, "x"), "x?"),
                            problem(InsightSeverity::Error, find(text, "y"), "y?"),
                            problem(InsightSeverity::Warning, find(text, "c"), "unused c")});
    const InsightSummary summary = h.view->insightSummary();
    EXPECT_EQ(summary.count(InsightSeverity::Error), 2);
    EXPECT_EQ(summary.count(InsightSeverity::Warning), 1);
    EXPECT_EQ(describe(summary), "2 errors, 1 warning");
    list = h.view->buildDisplayList();
    EXPECT_EQ(list.inspection.state, InspectionState::Problems);
    EXPECT_EQ(list.inspection.summary, summary);
    // In the top-right corner, left of the stripe.
    EXPECT_LE(list.inspection.rect.right(), list.stripe.left());
    EXPECT_GT(list.inspection.rect.left(), list.viewport.width() / 2.0);
    EXPECT_LT(list.inspection.rect.top(), 10.0);
    std::vector<QString> counts;
    for (const DisplayText& run : list.chromeText) {
        counts.push_back(run.text);
    }
    EXPECT_EQ(counts, (std::vector<QString>{QStringLiteral("2"), QStringLiteral("1")}));
    ASSERT_FALSE(list.inspection.next.isEmpty());
    ASSERT_FALSE(list.inspection.previous.isEmpty());
    EXPECT_EQ(h.view->toolTipAt(list.inspection.rect.center()).isEmpty(), false);

    // The arrows: down goes to the next error, up to the previous one (wrapping).
    EXPECT_TRUE(h.view->clickInsight(list.inspection.next.center()));
    EXPECT_EQ(h.caret(), find(text, "x").begin.value());
    EXPECT_TRUE(h.view->clickInsight(list.inspection.previous.center()));
    EXPECT_EQ(h.caret(), find(text, "y").begin.value());
    int problemsView = 0;
    QObject::connect(h.view.get(), &EditorView::problemsViewRequested, [&problemsView] { ++problemsView; });
    click(*h.view, QPointF(list.inspection.rect.left() + 2.0, list.inspection.rect.center().y())); // The counts.
    EXPECT_EQ(problemsView, 1);

    // The status-bar indicator follows the view.
    InsightStatusIndicator indicator;
    indicator.setView(h.view.get());
    EXPECT_EQ(indicator.text(), QStringLiteral("2 errors, 1 warning"));
    int requested = 0;
    QObject::connect(&indicator, &InsightStatusIndicator::problemsRequested, [&requested] { ++requested; });
    indicator.click();
    EXPECT_EQ(requested, 1);
    h.view->setDiagnostics(EditorDiagnosticSource::Live, {});
    EXPECT_EQ(indicator.text(), QStringLiteral("No problems"));
    EXPECT_EQ(h.view->buildDisplayList().inspection.state, InspectionState::Clean);
    EXPECT_GE(changed, 3);
}

TEST(EditorInsight, F2GoesToTheNextErrorWithABalloon) {
    FROST_REQUIRE_OFFSCREEN();
    constexpr std::string_view text = "int w = 0;\nint a = x;\nint b = y;\n";
    EditorHarness h(text);
    EXPECT_FALSE(h.view->gotoNextProblem(true)); // No problems: nothing moves.
    h.view->setDiagnostics(EditorDiagnosticSource::Live,
                           {problem(InsightSeverity::Warning, find(text, "w"), "unused w"),
                            problem(InsightSeverity::Error, find(text, "x"), "use of undeclared identifier 'x'"),
                            problem(InsightSeverity::Error, find(text, "y"), "use of undeclared identifier 'y'")});
    h.press(Qt::Key_F2);
    EXPECT_EQ(h.caret(), find(text, "x").begin.value()); // Errors before warnings.
    EXPECT_EQ(h.view->problemBalloonText(), QStringLiteral("use of undeclared identifier 'x'"));
    const EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_TRUE(list.balloon.has_value());
    EXPECT_EQ(list.balloon->severity, InsightSeverity::Error);
    EXPECT_GT(list.balloon->rect.top(), 0.0);
    EXPECT_TRUE(std::ranges::any_of(list.chromeText, [](const DisplayText& run) {
        return run.text == QStringLiteral("use of undeclared identifier 'x'");
    }));
    h.press(Qt::Key_F2);
    EXPECT_EQ(h.caret(), find(text, "y").begin.value());
    h.press(Qt::Key_F2); // Wraps.
    EXPECT_EQ(h.caret(), find(text, "x").begin.value());
    h.press(Qt::Key_F2, Qt::ShiftModifier); // Back, wrapping to the last error.
    EXPECT_EQ(h.caret(), find(text, "y").begin.value());
    h.press(Qt::Key_Escape);
    EXPECT_TRUE(h.view->problemBalloonText().isEmpty());

    // Only warnings left: F2 visits them.
    h.view->setDiagnostics(EditorDiagnosticSource::Live,
                           {problem(InsightSeverity::Warning, find(text, "w"), "unused w")});
    h.press(Qt::Key_F2);
    EXPECT_EQ(h.caret(), find(text, "w").begin.value());
    EXPECT_EQ(h.view->problemBalloonText(), QStringLiteral("unused w"));
    h.press(Qt::Key_Right); // A caret move hides the balloon.
    EXPECT_TRUE(h.view->problemBalloonText().isEmpty());
}

} // namespace
} // namespace frost::ui
