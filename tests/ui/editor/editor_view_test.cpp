// SPDX-License-Identifier: Apache-2.0
//
// EditorView tests (offscreen): typing and repainting, caret movement and Shift selection through
// the IntelliJ keymap, soft wrap, the gutter's numbers and markers, IME composition, the mouse,
// line copy and paste, read-only, status signals and the InputToPaint marker. They read the model
// and the display list; no pixels are compared.

#include "frost/ui/editor/display_list.hpp"

#include "editor/editor_harness.hpp"
#include <QClipboard>
#include <QInputMethodEvent>
#include <QMouseEvent>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace frost::ui {
namespace {

using test::EditorHarness;
using test::HarnessOptions;

// A viewport point just right of the boundary before `column` (bytes) on display row `row`, so a
// click there lands on that boundary.
QPointF cellPoint(EditorView& view, std::uint32_t row, std::uint32_t column) {
    const EditorDisplayList list = view.buildDisplayList();
    const text::TextWidth x =
        view.editor().display().xAt(text::DisplayPoint{text::DisplayRow{row}, text::DisplayColumn{column}});
    const double x0 = list.textArea.left() + 6.0; // kTextPadding at 100 %.
    return QPointF(x0 + toPixels(x) + 1.0, (static_cast<double>(row) + 0.5) * view.lineHeight());
}

// Counts the emissions of one signal (QSignalSpy needs QtTest, which the build does not link).
struct Counter {
    int count = 0;
    QList<QVariant> last;
};

void click(EditorView& view, QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent press(QEvent::MouseButtonPress, at, view.viewport()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      modifiers);
    QCoreApplication::sendEvent(view.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view.viewport()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                        modifiers);
    QCoreApplication::sendEvent(view.viewport(), &release);
}

TEST(EditorView, TypingAppearsInTheDocumentAndRepaints) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.settings.lineSeparator = "lf"; // A new document's first break ("system" is CRLF on Windows).
    EditorHarness h("", options);
    h.type("ab c");
    EXPECT_EQ(h.text(), "ab c");
    EXPECT_EQ(h.caret(), 4U);
    const std::uint64_t before = h.view->paintCount();
    h.view->viewport()->repaint();
    EXPECT_GT(h.view->paintCount(), before);
    const EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_NE(list.findText(QStringLiteral("ab c")), nullptr);
    // Enter and Backspace go through the keymap too.
    h.press(Qt::Key_Return);
    h.type("d");
    EXPECT_EQ(h.text(), "ab c\nd");
    h.press(Qt::Key_Backspace);
    h.press(Qt::Key_Backspace);
    EXPECT_EQ(h.text(), "ab c");
    // Undo and redo.
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_NE(h.text(), "ab c");
    h.press(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "ab c");
}

TEST(EditorView, FoldedTextIsPaintedAndNavigationRevealsIt) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("before {\nsecret\n}\nafter\n");
    ASSERT_TRUE(h.view->setCollapsedFolds({{ByteOffset{8}, ByteOffset{17}}, {ByteOffset{9}, ByteOffset{15}}}));
    const auto list = h.view->buildDisplayList();
    EXPECT_NE(list.findText(QStringLiteral("...")), nullptr);
    EXPECT_EQ(list.findText(QStringLiteral("secret")), nullptr);
    EXPECT_TRUE(h.view->toolTipAt(cellPoint(*h.view, 0, 9)).contains(QStringLiteral("secret")));
    EXPECT_FALSE(h.view->status().dirty);
    h.view->goToLine(2, 2);
    EXPECT_TRUE(h.view->collapsedFolds().empty());
    EXPECT_NE(h.view->buildDisplayList().findText(QStringLiteral("secret")), nullptr);
}

TEST(EditorView, KeyboardSelectionFoldAndDeleteReveal) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("hello world");
    const std::array selections{text::Selection{ByteOffset{0}, ByteOffset{5}}};
    h.view->setState(text::EditorState::from(text::SelectionSet::fromSelections(selections, 0)));
    h.press(Qt::Key_Period, Qt::ControlModifier);
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.caret(), 0U);
    h.press(Qt::Key_Delete);
    EXPECT_EQ(h.text(), "hello world");
    EXPECT_TRUE(h.view->collapsedFolds().empty());
    h.press(Qt::Key_Delete);
    EXPECT_EQ(h.text(), "ello world");
}

TEST(EditorView, FoldRejectsSplitGraphemesAtomically) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("a\xCC\x81 xyz");
    ASSERT_TRUE(h.view->setCollapsedFolds({{ByteOffset{0}, ByteOffset{3}}}));
    EXPECT_FALSE(h.view->setCollapsedFolds({{ByteOffset{1}, ByteOffset{3}}}));
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.view->collapsedFolds().front().begin, ByteOffset{0});
    EXPECT_EQ(h.view->collapsedFolds().front().end, ByteOffset{3});
}

TEST(EditorView, CombiningInsertionAtFoldEndRevalidatesGraphemeBoundary) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("a b");
    ASSERT_TRUE(h.view->setCollapsedFolds({{ByteOffset{0}, ByteOffset{1}}}));
    const std::array selections{text::Selection{ByteOffset{1}, ByteOffset{1}}};
    h.view->setState(text::EditorState::from(text::SelectionSet::fromSelections(selections, 0)));
    h.press(0, Qt::NoModifier, QString(QChar(0x0301)));
    EXPECT_EQ(h.text(), "a\xCC\x81 b");
    EXPECT_TRUE(h.view->collapsedFolds().empty());
}

TEST(EditorView, EditingOutsideFoldAndUndoKeepFoldAttached) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("top\n{\ninside\n}\nend\n");
    ASSERT_TRUE(h.view->setCollapsedFolds({{ByteOffset{5}, ByteOffset{14}}}));
    h.type("x");
    EXPECT_TRUE(h.view->status().dirty);
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.view->collapsedFolds().front().begin, ByteOffset{6});
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "top\n{\ninside\n}\nend\n");
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.view->collapsedFolds().front().begin, ByteOffset{5});
}

TEST(EditorView, SyntaxFoldGutterTogglesNestedCode) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    EditorHarness h("int main() {\n  if (true) {\n    return 1;\n  }\n}\n", options);
    h.settle();
    const auto gutterRight = h.view->buildDisplayList().textArea.left();
    click(*h.view, QPointF(gutterRight - 7.0, h.view->lineHeight() / 2.0));
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.view->buildDisplayList().findText(QStringLiteral("return 1;")), nullptr);
    click(*h.view, QPointF(gutterRight - 7.0, h.view->lineHeight() / 2.0));
    EXPECT_TRUE(h.view->collapsedFolds().empty());
}

TEST(EditorView, FoldAllOrdersCandidatesAcrossWorkerChunks) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    std::string source;
    for (int i = 0; i < 150; ++i)
        source += "void f" + std::to_string(i) + "() {\n}\n";
    EditorHarness h(source, options);
    h.settle();
    ASSERT_TRUE(h.view->perform(EditorAction::FoldAll));
    ASSERT_EQ(h.view->collapsedFolds().size(), 150U);
    EXPECT_TRUE(std::ranges::is_sorted(h.view->collapsedFolds(), {}, &text::ByteRange::begin));
}

TEST(EditorView, StatusPositionsRestoreGraphemesWithoutChangingByteNavigation) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("A\xF0\x9F\x98\x80"
                    "e\xCC\x81"
                    "\xC3\xA9\tZ");
    const std::array<std::uint64_t, 7> offsets{0, 1, 5, 8, 10, 11, 12};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        h.view->setState(text::EditorState::caretAt(ByteOffset{offsets[i]}));
        const auto saved = h.view->status();
        EXPECT_EQ(saved.column, static_cast<int>(i + 1));
        h.view->goToStatusPosition(saved.line, saved.column);
        EXPECT_EQ(h.caret(), offsets[i]);
    }
    h.view->goToLine(1, 9); // External navigation still supplies a byte column.
    EXPECT_EQ(h.caret(), 8U);
}

TEST(EditorView, LongLineStatusPositionRestoresScalarCount) {
    FROST_REQUIRE_OFFSCREEN();
    std::string source(4095, 'x');
    source += "e\xCC\x81"
              "\xF0\x9F\x98\x80Z";
    EditorHarness h(source);
    const auto target = static_cast<std::uint64_t>(source.size() - 1);
    h.view->setState(text::EditorState::caretAt(ByteOffset{target}));
    const auto saved = h.view->status();
    h.view->goToStatusPosition(saved.line, saved.column);
    EXPECT_EQ(h.caret(), target);
}

TEST(EditorView, CaretMovesWithTheNavigationKeys) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("hello world\nsecond line\nthird");
    h.press(Qt::Key_Right);
    h.press(Qt::Key_Right);
    EXPECT_EQ(h.caret(), 2U);
    h.press(Qt::Key_Down);
    EXPECT_EQ(h.view->status().line, 2);
    EXPECT_EQ(h.view->status().column, 3);
    h.press(Qt::Key_End);
    EXPECT_EQ(h.caret(), 23U); // The end of "second line".
    h.press(Qt::Key_Home);
    EXPECT_EQ(h.caret(), 12U);
    h.press(Qt::Key_Right, Qt::ControlModifier);
    EXPECT_EQ(h.caret(), 18U); // Past "second".
    h.press(Qt::Key_End, Qt::ControlModifier);
    EXPECT_EQ(h.caret(), h.document.text().size().value());
    h.press(Qt::Key_Home, Qt::ControlModifier);
    EXPECT_EQ(h.caret(), 0U);
    // Up on the first row and Down on the last row keep the caret (ADR 0025).
    h.press(Qt::Key_Up);
    EXPECT_EQ(h.caret(), 0U);
    h.press(Qt::Key_PageDown);
    EXPECT_EQ(h.view->status().line, 3);
}

TEST(EditorView, ShiftExtendsTheSelection) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("alpha beta\ngamma");
    Counter selection;
    QObject::connect(h.view.get(), &EditorView::selectionChanged, [&](int carets, int characters) {
        ++selection.count;
        selection.last = {carets, characters};
    });
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{3}}));
    EXPECT_EQ(h.view->status().selectedCharacters, 3);
    EXPECT_GT(selection.count, 0);
    EXPECT_EQ(selection.last, (QList<QVariant>{1, 3}));
    EXPECT_TRUE(h.view->hasSelection());
    EXPECT_TRUE(h.view->contextKeys().has(input::ContextKey::EditorHasSelection));
    h.press(Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{14}}));
    // The selection shows as a fill; typing replaces it.
    const EditorDisplayList list = h.view->buildDisplayList();
    EXPECT_FALSE(list.fills.empty());
    h.type("X");
    EXPECT_EQ(h.text(), "Xma");
    h.press(Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(h.view->status().selectedCharacters, 3);
}

TEST(EditorView, SoftWrapChangesTheRowCount) {
    FROST_REQUIRE_OFFSCREEN();
    const std::string longLine(400, 'w');
    EditorHarness h(longLine + "\nshort");
    EXPECT_EQ(h.view->editor().display().rowCount(), 2U);
    EditorViewSettings settings = h.view->settings();
    settings.softWrap = true;
    h.view->applySettings(settings);
    EXPECT_GT(h.view->editor().display().rowCount(), 3U);
    settings.softWrap = false;
    h.view->applySettings(settings);
    EXPECT_EQ(h.view->editor().display().rowCount(), 2U);
}

TEST(EditorView, GutterShowsLineNumbersAndMarkers) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\ntwo\nthree\n");
    EditorDisplayList list = h.view->buildDisplayList();
    for (int number = 1; number <= 4; ++number) {
        EXPECT_NE(list.findLineNumber(number), nullptr) << number;
    }
    EXPECT_EQ(list.findLineNumber(5), nullptr);
    EXPECT_TRUE(list.markers.empty());

    h.view->setLineMarkers({LineMarker{LineIndex{1}, MarkerSeverity::Warning, "unused"},
                            LineMarker{LineIndex{1}, MarkerSeverity::Error, "no such name"},
                            LineMarker{LineIndex{2}, MarkerSeverity::Warning, "shadowed"}});
    list = h.view->buildDisplayList();
    ASSERT_EQ(list.markers.size(), 2U);
    EXPECT_EQ(list.markers[0].line, LineIndex{1});
    EXPECT_EQ(list.markers[0].severity, MarkerSeverity::Error); // The most severe of the line.
    EXPECT_EQ(list.markers[0].color, h.view->colors().errorMarker);
    EXPECT_EQ(list.markers[1].severity, MarkerSeverity::Warning);
    EXPECT_TRUE(list.gutter.contains(list.markers[0].rect));
    // The gutter tooltip lists the line's messages; the text area has none.
    const double secondRow = 1.5 * h.view->lineHeight();
    EXPECT_EQ(h.view->lineMarkerToolTip(QPointF(2.0, secondRow)), QStringLiteral("unused\nno such name"));
    EXPECT_TRUE(h.view->lineMarkerToolTip(QPointF(list.textArea.left() + 20.0, secondRow)).isEmpty());

    EditorViewSettings settings = h.view->settings();
    settings.lineNumbers = false;
    h.view->applySettings(settings);
    list = h.view->buildDisplayList();
    EXPECT_EQ(list.findLineNumber(1), nullptr);
    EXPECT_EQ(list.markers.size(), 2U);
}

TEST(EditorView, GutterShowsVcsChangeStripesAndReportsTheirClicks) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\ntwo\nthree\nfour\n");
    EXPECT_TRUE(h.view->buildDisplayList().changeStripes.empty());

    // Line 1 added, lines 2-3 changed, a deletion before line 4 (unsorted on purpose).
    h.view->setLineChanges({LineChangeMarker{LineIndex{4}, 0, ChangeStripeKind::Deleted},
                            LineChangeMarker{LineIndex{0}, 1, ChangeStripeKind::Added},
                            LineChangeMarker{LineIndex{1}, 2, ChangeStripeKind::Modified}});
    ASSERT_EQ(h.view->lineChanges().size(), 3U);
    EXPECT_EQ(h.view->lineChanges()[0].kind, ChangeStripeKind::Added);
    const EditorDisplayList list = h.view->buildDisplayList();
    // One stripe per row of an added or changed line, and one mark for the deletion.
    ASSERT_EQ(list.changeStripes.size(), 4U);
    EXPECT_EQ(list.changeStripes[0].kind, ChangeStripeKind::Added);
    EXPECT_EQ(list.changeStripes[0].color, h.view->colors().changeAdded);
    EXPECT_EQ(list.changeStripes[1].kind, ChangeStripeKind::Modified);
    EXPECT_EQ(list.changeStripes[1].color, h.view->colors().changeModified);
    EXPECT_EQ(list.changeStripes[2].line, LineIndex{2});
    EXPECT_EQ(list.changeStripes[3].kind, ChangeStripeKind::Deleted);
    EXPECT_EQ(list.changeStripes[3].change, 2U);
    for (const DisplayChangeStripe& stripe : list.changeStripes) {
        EXPECT_TRUE(list.gutter.contains(stripe.rect));
    }

    // A click on a stripe reports the change and leaves the caret alone; elsewhere it does not.
    std::vector<int> clicked;
    QObject::connect(h.view.get(), &EditorView::lineChangeClicked,
                     [&clicked](int index, QPoint) { clicked.push_back(index); });
    const std::uint64_t caret = h.caret();
    click(*h.view, list.changeStripes[1].rect.center());
    EXPECT_EQ(clicked, std::vector<int>{1});
    EXPECT_EQ(h.caret(), caret);
    EXPECT_EQ(h.view->lineChangeAt(list.changeStripes[0].rect.center()), std::optional<std::size_t>(0));
    EXPECT_EQ(h.view->lineChangeAt(QPointF(list.textArea.left() + 20.0, list.changeStripes[0].rect.center().y())),
              std::nullopt);

    // Typing emits textChanged (the markers' debounce follows it).
    int changed = 0;
    QObject::connect(h.view.get(), &EditorView::textChanged, [&changed] { ++changed; });
    h.type("x");
    EXPECT_EQ(changed, 1);

    h.view->setLineChanges({});
    EXPECT_TRUE(h.view->buildDisplayList().changeStripes.empty());
}

TEST(EditorView, RangeDecorationsUnderlineTheirTextWithATooltip) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("int missing;\nreturn x;\n");
    EXPECT_TRUE(h.view->buildDisplayList().decorations.empty());
    // A wavy error under "missing" (bytes 4..11) and a straight warning under "x" (bytes 20..21),
    // through editor-insight's per-owner decorations (the W1 workbench's range decorations before
    // the merge; given unsorted on purpose).
    h.view->setDecorations(
        "w1", {EditorDecoration{text::ByteRange{ByteOffset{20}, ByteOffset{21}}, DecorationStyle::Underline,
                                InsightSeverity::Warning, QColor(), "unused"},
               EditorDecoration{text::ByteRange{ByteOffset{4}, ByteOffset{11}}, DecorationStyle::WavyUnderline,
                                InsightSeverity::Error, QColor(), "use of undeclared identifier"}});
    EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_EQ(list.decorations.size(), 2U);
    const DisplayDecoration& error = list.decorations[0]; // Row by row.
    EXPECT_EQ(error.style, DecorationStyle::WavyUnderline);
    EXPECT_EQ(error.color, h.view->colors().errorMarker);
    EXPECT_GE(error.rect.left(), list.textArea.left());
    EXPECT_GT(error.rect.right(), error.rect.left());
    EXPECT_LT(error.baseline, h.view->lineHeight()); // On the first row.
    EXPECT_EQ(list.decorations[1].style, DecorationStyle::Underline);
    EXPECT_EQ(list.decorations[1].color, h.view->colors().warningMarker);
    EXPECT_GT(list.decorations[1].baseline, h.view->lineHeight()); // On the second row.

    // The tooltip names the message under the pointer, nothing elsewhere.
    const QPointF over(error.rect.center().x(), 0.5 * h.view->lineHeight());
    EXPECT_EQ(h.view->toolTipAt(over), QStringLiteral("use of undeclared identifier"));
    EXPECT_TRUE(h.view->toolTipAt(QPointF(error.rect.right() + 40.0, 0.5 * h.view->lineHeight())).isEmpty());

    h.view->setDecorations("w1", {});
    EXPECT_TRUE(h.view->buildDisplayList().decorations.empty());
}

// The debugger's gutter (ADR 0050): breakpoint discs in place of the line number, the hollow grey
// one with its reason as the tooltip, the execution arrow with the stopped line highlighted, and a
// click in the breakpoint area asking for a toggle without moving the caret.
TEST(EditorView, BreakpointsAndTheExecutionPointInTheGutter) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\ntwo\nthree\n");
    const EditorColors& colors = h.view->colors();
    const auto lineFills = [&](const EditorDisplayList& list) {
        return std::ranges::count_if(list.fills,
                                     [&](const DisplayFill& fill) { return fill.color == colors.executionLine; });
    };
    EditorDisplayList list = h.view->buildDisplayList();
    EXPECT_TRUE(list.breakpoints.empty());
    EXPECT_FALSE(list.executionPoint.has_value());
    EXPECT_FALSE(h.view->breakpointGutterEnabled());
    std::vector<int> toggles;
    QObject::connect(h.view.get(), &EditorView::breakpointToggleRequested, [&](int line) { toggles.push_back(line); });
    click(*h.view, QPointF(2.0, 1.5 * h.view->lineHeight()));
    EXPECT_TRUE(toggles.empty()) << "no breakpoint area until the debugger enables it";

    h.view->setBreakpointGutterEnabled(true);
    h.view->setBreakpointMarkers(
        {BreakpointMarker{LineIndex{0}, BreakpointKind::Verified, ""},
         BreakpointMarker{LineIndex{2}, BreakpointKind::Unverified, "No code is generated for this line"}});
    h.view->setExecutionPoint(LineIndex{1});
    EXPECT_EQ(h.view->executionPoint(), std::optional<LineIndex>(LineIndex{1}));
    EXPECT_EQ(h.view->breakpointMarkers().size(), 2U);
    list = h.view->buildDisplayList();
    ASSERT_EQ(list.breakpoints.size(), 2U);
    EXPECT_EQ(list.breakpoints[0].line, LineIndex{0});
    EXPECT_EQ(list.breakpoints[0].kind, BreakpointKind::Verified);
    EXPECT_EQ(list.breakpoints[0].color, colors.breakpoint);
    EXPECT_EQ(list.breakpoints[1].kind, BreakpointKind::Unverified);
    EXPECT_EQ(list.breakpoints[1].color, colors.breakpointUnverified);
    EXPECT_NE(colors.breakpoint, colors.breakpointUnverified);
    EXPECT_TRUE(list.gutter.contains(list.breakpoints[0].rect));
    EXPECT_EQ(list.findLineNumber(1), nullptr) << "the disc replaces the line number";
    EXPECT_EQ(list.findLineNumber(2), nullptr) << "so does the execution arrow";
    EXPECT_NE(list.findLineNumber(4), nullptr);
    ASSERT_TRUE(list.executionPoint.has_value());
    EXPECT_EQ(list.executionPoint->line, LineIndex{1});
    EXPECT_EQ(list.executionPoint->color, colors.executionPoint);
    EXPECT_TRUE(list.gutter.contains(list.executionPoint->rect));
    EXPECT_EQ(lineFills(list), 1) << "the stopped line is highlighted";
    EXPECT_EQ(h.view->lineMarkerToolTip(QPointF(2.0, 2.5 * h.view->lineHeight())),
              QStringLiteral("No code is generated for this line"));

    // A click on a line's number area asks for a toggle (0-based line) and keeps the caret.
    const std::uint64_t caret = h.caret();
    EXPECT_EQ(h.view->breakpointLineAt(list.breakpoints[1].rect.center()), std::optional<LineIndex>(LineIndex{2}));
    click(*h.view, list.breakpoints[1].rect.center());
    click(*h.view, QPointF(2.0, 1.5 * h.view->lineHeight()));
    EXPECT_EQ(toggles, (std::vector<int>{2, 1}));
    EXPECT_EQ(h.caret(), caret);
    EXPECT_FALSE(h.view->breakpointLineAt(QPointF(list.textArea.left() + 5.0, 5.0)).has_value())
        << "the text area is not the breakpoint area";

    h.view->setExecutionPoint(std::nullopt);
    h.view->setBreakpointMarkers({});
    list = h.view->buildDisplayList();
    EXPECT_TRUE(list.breakpoints.empty());
    EXPECT_FALSE(list.executionPoint.has_value());
    EXPECT_EQ(lineFills(list), 0);
    EXPECT_NE(list.findLineNumber(1), nullptr);
}

TEST(EditorView, FoldGutterCommitsImeBeforeCollapsing) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    constexpr std::string_view source = "int main() {\n  return 1;\n}\n";
    EditorHarness h(source, options);
    QInputMethodEvent preedit(QStringLiteral(" "), {});
    QCoreApplication::sendEvent(h.view.get(), &preedit);
    ASSERT_TRUE(h.document.compositionRange());
    h.settle();
    h.settle();
    ASSERT_TRUE(std::ranges::any_of(h.view->buildDisplayList().gutterText, [](const DisplayText& run) {
        return run.text == QString(QChar(0x2212));
    })) << "no syntax fold candidate before the fold action";
    const auto gutterRight = h.view->buildDisplayList().textArea.left();
    click(*h.view, QPointF(gutterRight - 7.0, h.view->lineHeight() / 2.0));
    EXPECT_FALSE(h.document.compositionRange());
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.text(), " " + std::string(source));
    EXPECT_EQ(h.caret(), 1U);
    h.view->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_TRUE(h.view->hasFocus());
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), source);
    EXPECT_EQ(h.caret(), 0U);
    EXPECT_FALSE(h.document.compositionRange());
}

TEST(EditorView, FoldActionsCommitImeBeforeChangingFolds) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    constexpr std::string_view source = "int main() {\n  return 1;\n}\n";
    EditorHarness h(source, options);
    QInputMethodEvent preedit(QStringLiteral(" "), {});
    QCoreApplication::sendEvent(h.view.get(), &preedit);
    ASSERT_TRUE(h.document.compositionRange());
    h.settle();
    h.settle();
    ASSERT_TRUE(std::ranges::any_of(h.view->buildDisplayList().gutterText, [](const DisplayText& run) {
        return run.text == QString(QChar(0x2212));
    })) << "no syntax fold candidate before the fold action";
    ASSERT_TRUE(h.view->perform(EditorAction::FoldAll));
    EXPECT_FALSE(h.document.compositionRange());
    ASSERT_EQ(h.view->collapsedFolds().size(), 1U);
    EXPECT_EQ(h.text(), " " + std::string(source));
    EXPECT_EQ(h.caret(), 1U);
    h.view->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_TRUE(h.view->hasFocus());
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), source);
    EXPECT_EQ(h.caret(), 0U);
    EXPECT_FALSE(h.document.compositionRange());
}

TEST(EditorView, FoldGutterConsumesClickWhenImeChangesSyntax) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    constexpr std::string_view source = "int main() {\n  return 1;\n}\n";
    EditorHarness h(source, options);
    const std::array selections{text::Selection{ByteOffset{0}, ByteOffset{3}}};
    h.view->setState(text::EditorState::from(text::SelectionSet::fromSelections(selections, 0)));
    QInputMethodEvent preedit(QStringLiteral("x"), {});
    QCoreApplication::sendEvent(h.view.get(), &preedit);
    h.settle();
    h.settle();
    const auto gutterRight = h.view->buildDisplayList().textArea.left();
    click(*h.view, QPointF(gutterRight - 7.0, h.view->lineHeight() / 2.0));
    EXPECT_FALSE(h.document.compositionRange());
    EXPECT_EQ(h.text(), "x main() {\n  return 1;\n}\n");
    EXPECT_TRUE(h.view->collapsedFolds().empty());
    EXPECT_EQ(h.caret(), 1U);
    h.view->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_TRUE(h.view->hasFocus());
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), source);
    EXPECT_EQ(h.caret(), 3U);
}

TEST(EditorView, FoldActionWaitsForSyntaxAfterImeReplacesSelection) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions options;
    options.path = "test.cpp";
    options.syntax = true;
    constexpr std::string_view source = "int main() {\n  return 1;\n}\n";
    EditorHarness h(source, options);
    const std::array selections{text::Selection{ByteOffset{0}, ByteOffset{3}}};
    h.view->setState(text::EditorState::from(text::SelectionSet::fromSelections(selections, 0)));
    QInputMethodEvent preedit(QStringLiteral("x"), {});
    QCoreApplication::sendEvent(h.view.get(), &preedit);
    ASSERT_TRUE(h.document.compositionRange());
    EXPECT_FALSE(h.view->perform(EditorAction::FoldAll));
    EXPECT_FALSE(h.document.compositionRange());
    EXPECT_EQ(h.text(), "x main() {\n  return 1;\n}\n");
    EXPECT_TRUE(h.view->collapsedFolds().empty());
    h.view->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_TRUE(h.view->hasFocus());
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), source);
    EXPECT_EQ(h.caret(), 3U);
}

TEST(EditorView, ImeCommitAndNewPreeditKeepsShortcutSuppression) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("");
    QInputMethodEvent first(QStringLiteral("a"), {});
    QCoreApplication::sendEvent(h.view.get(), &first);
    QInputMethodEvent continued(QStringLiteral("b"), {});
    continued.setCommitString(QStringLiteral("a"));
    QCoreApplication::sendEvent(h.view.get(), &continued);
    ASSERT_TRUE(h.document.compositionRange());
    ASSERT_EQ(h.text(), "ab");
    h.view->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_TRUE(h.view->hasFocus());
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "ab");
    EXPECT_TRUE(h.document.compositionRange());
    QInputMethodEvent commit(QString(), {});
    commit.setCommitString(QStringLiteral("b"));
    QCoreApplication::sendEvent(h.view.get(), &commit);
    EXPECT_FALSE(h.document.compositionRange());
}

TEST(EditorView, ImeCompositionAndCommit) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("ab");
    h.press(Qt::Key_Right);
    {
        QInputMethodEvent preedit(QStringLiteral("に"), {});
        QCoreApplication::sendEvent(h.view.get(), &preedit);
    }
    EXPECT_EQ(h.text(), "aにb");
    EXPECT_TRUE(h.document.compositionRange().has_value());
    EXPECT_FALSE(h.view->buildDisplayList().overlays.empty()); // The preedit underline.
    {
        QInputMethodEvent commit(QString(), {});
        commit.setCommitString(QStringLiteral("日本"));
        QCoreApplication::sendEvent(h.view.get(), &commit);
    }
    EXPECT_EQ(h.text(), "a日本b");
    EXPECT_FALSE(h.document.compositionRange().has_value());
    EXPECT_EQ(h.caret(), 7U); // After the two 3-byte characters.
    EXPECT_TRUE(h.view->buildDisplayList().overlays.empty());
    // The commit is one undo step.
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "ab");
    // A commit without preedit types directly.
    QInputMethodEvent direct(QString(), {});
    direct.setCommitString(QStringLiteral("ä"));
    QCoreApplication::sendEvent(h.view.get(), &direct);
    EXPECT_EQ(h.text(), "aäb");
    EXPECT_TRUE(h.view->inputMethodQuery(Qt::ImEnabled).toBool());
    EXPECT_TRUE(h.view->inputMethodQuery(Qt::ImCursorRectangle).toRect().isValid());
}

TEST(EditorView, MouseClickDragDoubleAndTripleClick) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("alpha beta gamma\nsecond");
    click(*h.view, cellPoint(*h.view, 0, 7));
    EXPECT_EQ(h.caret(), 7U);
    // Shift+click extends from the caret.
    click(*h.view, cellPoint(*h.view, 0, 12), Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{7}, ByteOffset{12}}));
    // Drag: press, move to the second row, release.
    const QPointF from = cellPoint(*h.view, 0, 2);
    const QPointF to = cellPoint(*h.view, 1, 3);
    QMouseEvent press(QEvent::MouseButtonPress, from, h.view->viewport()->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &press);
    QMouseEvent move(QEvent::MouseMove, to, h.view->viewport()->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, h.view->viewport()->mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &release);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{2}, ByteOffset{20}}));
    // A double click selects the word under the mouse.
    const QPointF word = cellPoint(*h.view, 0, 8);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, word, h.view->viewport()->mapToGlobal(word), Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &dbl);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{6}, ByteOffset{10}}));
    // A third click right after selects the line, its break included.
    click(*h.view, word);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{17}}));
    // A click in the gutter selects that line too.
    click(*h.view, QPointF(2.0, 1.5 * h.view->lineHeight()));
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{17}, ByteOffset{23}}));
}

TEST(EditorView, LineCopyAndPasteWithoutSelection) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("first\nsecond\n");
    h.press(Qt::Key_C, Qt::ControlModifier); // No selection: copies the caret's line.
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("first\n"));
    h.press(Qt::Key_Down);
    h.press(Qt::Key_Right);
    h.press(Qt::Key_V, Qt::ControlModifier); // A line copy pastes above the caret's line.
    EXPECT_EQ(h.text(), "first\nfirst\nsecond\n");
    h.press(Qt::Key_X, Qt::ControlModifier); // Cut without selection: the caret's line.
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("second\n"));
    EXPECT_EQ(h.text(), "first\nfirst\n");
    // A selection copies and pastes as text.
    h.press(Qt::Key_Home, Qt::ControlModifier);
    h.press(Qt::Key_End, Qt::ShiftModifier);
    h.press(Qt::Key_C, Qt::ControlModifier);
    h.press(Qt::Key_End);
    h.press(Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "firstfirst\nfirst\n");
}

TEST(EditorView, EditingKeysAndActions) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("a\nb\nc");
    h.press(Qt::Key_D, Qt::ControlModifier); // Duplicate line.
    EXPECT_EQ(h.text(), "a\na\nb\nc");
    h.press(Qt::Key_Y, Qt::ControlModifier); // Delete line.
    EXPECT_EQ(h.text(), "a\nb\nc");
    h.press(Qt::Key_Home, Qt::ControlModifier);
    h.press(Qt::Key_Down, Qt::AltModifier | Qt::ShiftModifier); // Move line down.
    EXPECT_EQ(h.text(), "b\na\nc");
    h.press(Qt::Key_Up, Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "a\nb\nc");
    h.press(Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier); // Join lines.
    EXPECT_EQ(h.text(), "a b\nc");
    h.press(Qt::Key_Home);
    h.press(Qt::Key_Tab);
    EXPECT_EQ(h.text(), "    a b\nc");
    h.press(Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "a b\nc");
    h.press(Qt::Key_Delete);
    EXPECT_EQ(h.text(), " b\nc");
}

TEST(EditorView, ReadOnlyBlocksEditsButNotMovement) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("fixed");
    Counter readOnly;
    QObject::connect(h.view.get(), &EditorView::readOnlyChanged, [&](bool on) {
        ++readOnly.count;
        readOnly.last = {on};
    });
    EditorFileInfo info;
    info.readOnly = true;
    h.view->setFileInfo(info);
    EXPECT_EQ(readOnly.count, 1);
    EXPECT_TRUE(h.view->status().readOnly);
    h.type("x");
    h.press(Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "fixed");
    h.press(Qt::Key_End);
    EXPECT_EQ(h.caret(), 5U);
    EXPECT_FALSE(h.view->perform(EditorAction::Backspace));
}

TEST(EditorView, StatusSignalsAndDirtyFlag) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("x\r\ny");
    Counter caret;
    QObject::connect(h.view.get(), &EditorView::caretPositionChanged, [&](int line, int column) {
        ++caret.count;
        caret.last = {line, column};
    });
    Counter modified;
    QObject::connect(h.view.get(), &EditorView::modifiedChanged, [&](bool on) {
        ++modified.count;
        modified.last = {on};
    });
    EXPECT_EQ(h.view->status().lineSeparator, "CRLF");
    EXPECT_EQ(h.view->status().encoding, "UTF-8");
    EXPECT_FALSE(h.view->status().dirty);
    h.press(Qt::Key_Down);
    ASSERT_EQ(caret.count, 1);
    EXPECT_EQ(caret.last, (QList<QVariant>{2, 1}));
    h.type("z");
    EXPECT_TRUE(h.view->status().dirty);
    EXPECT_EQ(modified.count, 1);
    h.view->markSaved();
    EXPECT_FALSE(h.view->status().dirty);
    EXPECT_EQ(modified.count, 2);
    // Enter uses the document's line separator.
    h.press(Qt::Key_Return);
    EXPECT_EQ(h.text(), "x\r\nz\r\ny");
}

TEST(EditorView, InputToPaintMarkerReportsAfterThePaint) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("");
    std::vector<InputToPaintSample> samples;
    h.view->setInputToPaintSink([&](const InputToPaintSample& sample) { samples.push_back(sample); });
    h.type("q");
    EXPECT_TRUE(samples.empty());
    h.view->viewport()->repaint();
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_LE(samples[0].input, samples[0].paintEnd);
    EXPECT_EQ(samples[0].textVersion, h.document.text().version());
    h.view->viewport()->repaint(); // No input since: no sample.
    EXPECT_EQ(samples.size(), 1U);
}

TEST(EditorView, RasterRendererCachesGlyphRuns) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("cached line\nother line");
    auto* raster = dynamic_cast<RasterEditorRenderer*>(&h.view->renderer());
    ASSERT_NE(raster, nullptr);
    h.view->viewport()->repaint();
    const GlyphCacheStats first = raster->cacheStats();
    EXPECT_GT(first.misses, 0U);
    h.view->viewport()->repaint();
    const GlyphCacheStats second = raster->cacheStats();
    EXPECT_EQ(second.misses, first.misses); // Nothing changed: every run came from the cache.
    EXPECT_GT(second.hits, first.hits);
}

} // namespace
} // namespace frost::ui
