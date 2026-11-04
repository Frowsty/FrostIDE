// SPDX-License-Identifier: Apache-2.0
//
// EditorView feature tests (offscreen): tree-sitter highlight spans in theme colours, find and
// replace, Go to Line, comment line on en-US (Ctrl+/) and de-DE (Ctrl+-, ADR 0030), next/previous
// member, live settings, and the action table. Highlighting is checked as span-to-format mapping
// in the display list, not as pixels.

#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/editorconfig.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/editor/editor_colors.hpp"
#include "frost/ui/editor/editor_settings.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/go_to_line.hpp"

#include "editor/editor_harness.hpp"
#include "support/appearance_fixture.hpp"
#include <QLineEdit>
#include <QToolButton>
#include <QWheelEvent>

#include <set>
#include <string>

namespace frost::ui {
namespace {

using test::EditorHarness;
using test::HarnessOptions;

constexpr std::string_view kCppSample = "#include <vector>\n"
                                        "// A comment\n"
                                        "int answer() {\n"
                                        "    return 42;\n"
                                        "}\n"
                                        "struct Box {\n"
                                        "    int size() const { return 1; }\n"
                                        "};\n";

HarnessOptions cpp(AppearanceManager* appearance = nullptr) {
    HarnessOptions options;
    options.path = "sample.cpp";
    options.syntax = true;
    options.appearance = appearance;
    return options;
}

void sendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &down);
    QKeyEvent up(QEvent::KeyRelease, key, modifiers);
    QCoreApplication::sendEvent(target, &up);
}

TEST(EditorHighlight, LuaAndCppViewsUseTheirOwnSyntax) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions lua;
    lua.path = "main.LUA";
    lua.syntax = true;
    EditorHarness script("local function greet()\n  return true\nend\n", lua);
    EditorHarness source(kCppSample, cpp());
    script.settle();
    source.settle();
    EXPECT_EQ(script.view->syntaxLanguage(), syntax::LanguageId::Lua);
    EXPECT_EQ(source.view->syntaxLanguage(), syntax::LanguageId::Cpp);
    const EditorDisplayList list = script.view->buildDisplayList();
    const DisplayText* keyword = list.findText(QStringLiteral("return"));
    ASSERT_NE(keyword, nullptr);
    ASSERT_TRUE(keyword->kind.has_value());
    EXPECT_EQ(*keyword->kind, syntax::HighlightKind::Keyword);
}

TEST(EditorHighlight, LuaIndentUsesCurrentTreeAfterLargePrefix) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions lua;
    lua.path = "large.lua";
    lua.syntax = true;
    std::string initial;
    for (int i = 0; i < 9000; ++i) {
        initial += "-- padding\n";
    }
    initial += "if ready then";
    EditorHarness script(initial, lua);
    script.view->editor().setState(text::EditorState::caretAt(ByteOffset{initial.size()}));
    script.press(Qt::Key_Return);
    EXPECT_EQ(script.text(), initial + "\n    ");
    script.settle();
    ASSERT_NE(script.view->syntaxSession(), nullptr);
    EXPECT_EQ(script.view->syntaxSession()->tree().version(), script.document.text().version());

    std::string inComment = initial + "\n--[=[\nif hidden then";
    EditorHarness commented(inComment, lua);
    commented.view->editor().setState(text::EditorState::caretAt(ByteOffset{inComment.size()}));
    commented.press(Qt::Key_Return);
    EXPECT_EQ(commented.text(), inComment + "\n");
}

TEST(EditorHighlight, LuaIndentHandlesClosedAndIncompleteConstructsWithParseTree) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions lua;
    lua.path = "indent.lua";
    lua.syntax = true;
    const auto check = [&](std::string_view input, std::string_view suffix) {
        EditorHarness h(input, lua);
        ASSERT_NE(h.view->syntaxSession(), nullptr);
        ASSERT_FALSE(h.view->syntaxSession()->tree().empty());
        ASSERT_FALSE(h.view->syntaxSession()->tree().stale());
        h.view->editor().setState(text::EditorState::caretAt(ByteOffset{input.size()}));
        h.press(Qt::Key_Return);
        EXPECT_TRUE(h.text().ends_with(suffix)) << h.text();
    };
    check("function f()\n    if x then\n        work()\n    end", "    end\n    ");
    check("function f()\n    if x then\n        work()\n        end", "        end\n    ");
    check("-- start\n    local f = function() return 1 end; consume()", "consume()\n    ");
    check("local s = \"\\z\nif ready then", "if ready then\n");
    check("local s = \"x\\\r\nif ready then", "if ready then\r\n");
}

TEST(EditorHighlight, LuaIndentFallsBackWhenSyntaxTreeIsStale) {
    FROST_REQUIRE_OFFSCREEN();
    HarnessOptions lua;
    lua.path = "stale.lua";
    lua.syntax = true;
    lua.syntaxOptions.syncParseBudget = IClock::Duration::zero();
    const auto check = [&](const std::string& initial, const std::string& typed, const std::string& suffix) {
        EditorHarness h(initial, lua);
        ASSERT_NE(h.view->syntaxSession(), nullptr);
        ASSERT_FALSE(h.view->syntaxSession()->tree().stale());
        h.view->editor().setState(text::EditorState::caretAt(ByteOffset{initial.size()}));
        h.type(typed);
        ASSERT_TRUE(h.view->syntaxSession()->tree().stale());
        h.press(Qt::Key_Return);
        EXPECT_TRUE(h.text().ends_with(suffix)) << h.text();
    };
    check("local s = \"\\z\nif ready the", "n", "if ready then\n");
    check("local s = \"x\\\r\nif ready the", "n", "if ready then\r\n");
    check("function f()\n    if x then\n        work()\n        en", "d", "        end\n    ");
    check("-- start\n    local f = function() return 1 end; consume(", ")", "consume()\n    ");
}

TEST(EditorHighlight, CppSpansUseTheThemeTokenColours) {
    FROST_REQUIRE_OFFSCREEN();
    AppearanceSettings appearance;
    appearance.theme = "glacier-dark";
    const auto installed = test::install(appearance);
    ASSERT_TRUE(installed);
    EditorHarness h(kCppSample, cpp(installed->manager.get()));
    ASSERT_NE(h.view->syntaxSession(), nullptr);
    EXPECT_EQ(h.view->syntaxLanguage(), syntax::LanguageId::Cpp);

    const Theme& theme = *installed->manager->themeSnapshot();
    const EditorDisplayList list = h.view->buildDisplayList();
    const DisplayText* keyword = list.findText(QStringLiteral("return"));
    ASSERT_NE(keyword, nullptr);
    ASSERT_TRUE(keyword->kind.has_value());
    EXPECT_EQ(*keyword->kind, syntax::HighlightKind::Keyword);
    EXPECT_EQ(keyword->color, toQColor(theme.color(ColorKey::SyntaxKeyword)));
    EXPECT_NE(keyword->color, toQColor(theme.color(ColorKey::EditorForeground)));
    const DisplayText* comment = list.findText(QStringLiteral("// A comment"));
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(comment->kind, syntax::HighlightKind::Comment);
    EXPECT_EQ(comment->color, toQColor(theme.color(ColorKey::SyntaxComment)));
    const DisplayText* number = list.findText(QStringLiteral("42"));
    ASSERT_NE(number, nullptr);
    EXPECT_EQ(number->kind, syntax::HighlightKind::Number);
    EXPECT_EQ(list.background, toQColor(theme.color(ColorKey::EditorBackground)));

    // An edit keeps highlighting (the tree follows the edit; spans are queried again).
    h.press(Qt::Key_End, Qt::ControlModifier);
    h.type("int more = 7;");
    h.settle();
    const EditorDisplayList after = h.view->buildDisplayList();
    const DisplayText* seven = after.findText(QStringLiteral("7"));
    ASSERT_NE(seven, nullptr);
    EXPECT_EQ(seven->kind, syntax::HighlightKind::Number);
    const DisplayText* stillKeyword = after.findText(QStringLiteral("return"));
    ASSERT_NE(stillKeyword, nullptr);
    EXPECT_EQ(stillKeyword->kind, syntax::HighlightKind::Keyword);
}

TEST(EditorHighlight, EveryKindHasAThemeKeyAndPlainTextUsesTheForeground) {
    FROST_REQUIRE_OFFSCREEN();
    std::set<ColorKey> keys;
    for (std::size_t i = 0; i < syntax::kHighlightKindCount; ++i) {
        keys.insert(colorKeyFor(static_cast<syntax::HighlightKind>(i)));
    }
    EXPECT_GT(keys.size(), 20U);
    EXPECT_TRUE(isItalic(syntax::HighlightKind::DocComment));
    EXPECT_FALSE(isItalic(syntax::HighlightKind::Keyword));
    // A .txt file has no language: one plain run in the foreground colour.
    EditorHarness h("return 42;");
    EXPECT_EQ(h.view->syntaxSession(), nullptr);
    const EditorDisplayList list = h.view->buildDisplayList();
    const DisplayText* run = list.findText(QStringLiteral("return 42;"));
    ASSERT_NE(run, nullptr);
    EXPECT_FALSE(run->kind.has_value());
    EXPECT_EQ(run->color, h.view->colors().foreground);
}

TEST(EditorFind, FindRevealsHiddenMatch) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("a\nhidden\nz\n");
    ASSERT_TRUE(h.view->setCollapsedFolds({{ByteOffset{1}, ByteOffset{9}}}));
    h.view->openFind(false);
    h.view->findBar()->findField()->setText(QStringLiteral("hidden"));
    EXPECT_TRUE(h.view->collapsedFolds().empty());
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{2}, ByteOffset{8}}));
    EXPECT_NE(h.view->buildDisplayList().findText(QStringLiteral("hidden")), nullptr);
}

TEST(EditorFind, FindBarFindsCountsAndReplaces) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("foo bar Foo baz foo");
    h.press(Qt::Key_F, Qt::ControlModifier);
    ASSERT_TRUE(h.view->findBarVisible());
    EditorFindBar& bar = *h.view->findBar();
    EXPECT_FALSE(bar.replaceVisible());
    bar.findField()->setText(QStringLiteral("foo")); // Incremental: selects the first match.
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{3}}));
    EXPECT_EQ(bar.matchText(), QStringLiteral("1/3"));
    EXPECT_EQ(h.view->searchMatches().size(), 3U);
    // Matches are painted.
    EXPECT_GE(h.view->buildDisplayList().fills.size(), 3U);
    EXPECT_TRUE(h.view->findNext());
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{8}, ByteOffset{11}}));
    EXPECT_EQ(bar.matchText(), QStringLiteral("2/3"));
    // F3 / Shift+F3 in the find field.
    sendKey(bar.findField(), Qt::Key_F3);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{16}, ByteOffset{19}}));
    sendKey(bar.findField(), Qt::Key_F3, Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{8}, ByteOffset{11}}));
    // Match Case drops "Foo".
    bar.caseButton()->setChecked(true);
    EXPECT_EQ(h.view->searchMatches().size(), 2U);
    bar.caseButton()->setChecked(false);
    // Whole words and regex.
    bar.findField()->setText(QStringLiteral("ba[rz]"));
    bar.regexButton()->setChecked(true);
    EXPECT_EQ(h.view->searchMatches().size(), 2U);
    bar.regexButton()->setChecked(false);
    bar.findField()->setText(QStringLiteral("fo"));
    bar.wordButton()->setChecked(true);
    EXPECT_EQ(h.view->searchMatches().size(), 0U);
    EXPECT_EQ(bar.matchText(), QStringLiteral("0 results"));
    bar.wordButton()->setChecked(false);

    // Replace: Ctrl+R shows the replace row.
    h.view->closeFind();
    EXPECT_FALSE(h.view->findBarVisible());
    h.press(Qt::Key_Home, Qt::ControlModifier);
    h.press(Qt::Key_R, Qt::ControlModifier);
    ASSERT_TRUE(h.view->findBarVisible());
    EXPECT_TRUE(bar.replaceVisible());
    bar.findField()->setText(QStringLiteral("foo"));
    bar.replaceField()->setText(QStringLiteral("qux"));
    EXPECT_TRUE(h.view->replaceNext()); // The selected first match.
    EXPECT_EQ(h.text(), "qux bar Foo baz foo");
    EXPECT_EQ(h.view->replaceAll(), 2U);
    EXPECT_EQ(h.text(), "qux bar qux baz qux");
    // Replace All is one undo step.
    EXPECT_TRUE(h.view->perform(EditorAction::Undo));
    EXPECT_EQ(h.text(), "qux bar Foo baz foo");
    // Escape in the field closes the bar.
    sendKey(bar.findField(), Qt::Key_Escape);
    EXPECT_FALSE(h.view->findBarVisible());
}

TEST(EditorFind, BadRegexShowsAnError) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("text");
    h.view->openFind(false);
    h.view->findBar()->regexButton()->setChecked(true);
    h.view->findBar()->findField()->setText(QStringLiteral("(unclosed"));
    EXPECT_EQ(h.view->findBar()->matchText(), QStringLiteral("Bad pattern"));
    EXPECT_FALSE(h.view->findNext());
}

TEST(EditorGoToLine, CtrlGJumpsToLineAndColumn) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\ntwo\nthree\nfour\nfive");
    h.press(Qt::Key_G, Qt::ControlModifier);
    GoToLinePopup& popup = *h.view->goToLinePopup();
    ASSERT_FALSE(popup.isHidden());
    EXPECT_EQ(popup.field()->text(), QStringLiteral("1:1"));
    popup.field()->setText(QStringLiteral("abc"));
    sendKey(popup.field(), Qt::Key_Return);
    EXPECT_FALSE(popup.errorText().isEmpty());
    EXPECT_FALSE(popup.isHidden());
    popup.field()->setText(QStringLiteral("3:4"));
    sendKey(popup.field(), Qt::Key_Return);
    EXPECT_TRUE(popup.isHidden());
    EXPECT_EQ(h.view->status().line, 3);
    EXPECT_EQ(h.view->status().column, 4);
    // Escape cancels; the shell's goToLine clamps.
    h.view->openGoToLine();
    sendKey(popup.field(), Qt::Key_Escape);
    EXPECT_TRUE(popup.isHidden());
    h.view->goToLine(99, 1);
    EXPECT_EQ(h.view->status().line, 5);
}

TEST(EditorComment, CtrlSlashOnEnUsAndCtrlMinusOnDeDe) {
    FROST_REQUIRE_OFFSCREEN();
    {
        EditorHarness h("int a;\nint b;", cpp());
        h.press(Qt::Key_Slash, Qt::ControlModifier);
        EXPECT_EQ(h.text(), "// int a;\nint b;");
        h.press(Qt::Key_Up);
        h.press(Qt::Key_Slash, Qt::ControlModifier);
        EXPECT_EQ(h.text(), "int a;\nint b;");
        // Ctrl+Shift+/ wraps the selection in a block comment.
        h.press(Qt::Key_Home, Qt::ControlModifier);
        h.press(Qt::Key_End, Qt::ShiftModifier);
        h.press(Qt::Key_Slash, Qt::ControlModifier | Qt::ShiftModifier);
        EXPECT_EQ(h.text(), "/*int a;*/\nint b;");
    }
    {
        HarnessOptions options = cpp();
        options.layout = input::SimulatedLayoutId::DeDe;
        EditorHarness h("int a;\nint b;", options);
        // The German '-' key is the US '/' key (ADR 0030): Ctrl+- comments the line.
        h.press(Qt::Key_Minus, Qt::ControlModifier);
        EXPECT_EQ(h.text(), "// int a;\nint b;");
        // Typing still types: '-' without Ctrl inserts a minus.
        h.press(Qt::Key_Minus, Qt::NoModifier, QStringLiteral("-"));
        EXPECT_EQ(h.text(), "// int a;\n-int b;");
    }
}

TEST(EditorNavigation, AltDownAndAltUpJumpBetweenMembers) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h(kCppSample, cpp());
    const std::string text(kCppSample);
    const std::uint64_t answer = text.find("int answer");
    const std::uint64_t box = text.find("struct Box");
    const std::uint64_t size = text.find("int size");
    h.press(Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(h.caret(), answer);
    h.press(Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(h.caret(), box);
    h.press(Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(h.caret(), size);
    h.press(Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(h.caret(), box);
}

TEST(EditorSettings, StoreChangesUpdateFontAndTabSizeLive) {
    FROST_REQUIRE_OFFSCREEN();
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    const std::string appFile = "/home/u/.config/frostide/settings.json";
    ASSERT_TRUE(fs.createDirectories("/home/u/.config/frostide"));
    auto store = settings::SettingsStore::open(
        settings::builtinRegistry(), fs, settings::SettingsPaths{.applicationFile = appFile, .project = {}},
        settings::StoreOptions{.projectTrusted = false, .migrations = nullptr, .style = io::PathStyle::Posix});
    ASSERT_TRUE(store);
    EditorHarness h("\tx\ny");
    EditorSettingsBinding binding(**store, fs, "", *h.view);
    EXPECT_EQ(h.view->settings().fontSize, 13.0);
    EXPECT_DOUBLE_EQ(h.view->editorFont().pointSizeF(), 13.0);
    EXPECT_EQ(h.view->shaper().tabSize(), 4U);

    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorFontSize, std::int64_t{20}));
    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorTabSize, std::int64_t{2}));
    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorInsertSpaces, false));
    QCoreApplication::processEvents(); // The binding refreshes on the GUI thread.
    EXPECT_EQ(h.view->settings().fontSize, 20.0);
    EXPECT_DOUBLE_EQ(h.view->editorFont().pointSizeF(), 20.0);
    EXPECT_EQ(h.view->shaper().tabSize(), 2U);
    EXPECT_EQ(h.view->editor().config().settings.tabSize, 2U);
    // The tab is two cells wide now.
    const text::TextWidth tabEnd =
        h.view->editor().display().xAt(text::DisplayPoint{text::DisplayRow{0}, text::DisplayColumn{1}});
    EXPECT_EQ(tabEnd, 2 * h.view->shaper().cellWidth());
    EXPECT_TRUE(h.view->status().useTabs);
    h.press(Qt::Key_End);
    h.press(Qt::Key_Return);
    h.press(Qt::Key_Tab);
    EXPECT_EQ(h.text(), "\tx\n\t\t\ny"); // Enter keeps the tab indent; Tab inserts a tab.

    // Soft wrap and line numbers follow too.
    ASSERT_TRUE((*store)->set(settings::Scope::Application, settings::key::editorLineNumbers, false));
    QCoreApplication::processEvents();
    EXPECT_FALSE(h.view->settings().lineNumbers);
    EXPECT_EQ(h.view->buildDisplayList().findLineNumber(1), nullptr);
}

TEST(EditorSettings, EditorConfigOverlaysTheSnapshot) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/w/sub"));
    ASSERT_TRUE(fs.writeFileAtomic("/w/.editorconfig", "root = true\n[*.cpp]\nindent_style = tab\ntab_width = 8\n"));
    auto store = settings::SettingsStore::open(
        settings::builtinRegistry(), fs, settings::SettingsPaths{.applicationFile = "", .project = {}},
        settings::StoreOptions{.projectTrusted = false, .migrations = nullptr, .style = io::PathStyle::Posix});
    ASSERT_TRUE(store);
    const EditorViewSettings plain = editorViewSettings(*(*store)->snapshot());
    EXPECT_EQ(plain.fontFamily, "JetBrains Mono");
    EXPECT_EQ(plain.tabSize, 4U);
    EXPECT_TRUE(plain.insertSpaces);
    const auto file = settings::settingsForFile((*store)->snapshot(), fs, "/w/sub/a.cpp", io::PathStyle::Posix);
    ASSERT_TRUE(file);
    const EditorViewSettings overlaid = editorViewSettings(*file);
    EXPECT_EQ(overlaid.tabSize, 8U);
    EXPECT_FALSE(overlaid.insertSpaces);
}

TEST(EditorActions, TableIdsAreRegisteredAndBound) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("");
    input::ActionRegistry& registry = h.keys->registry();
    for (const EditorActionInfo& row : editorActions()) {
        const std::optional<input::ActionId> id = registry.find(row.id);
        ASSERT_TRUE(id.has_value()) << row.id;
        EXPECT_EQ(registry.info(*id).group, "Editor");
        EXPECT_FALSE(h.keys->keymap().shortcutsFor(*id).empty()) << row.id << " has no IntelliJ binding";
    }
    // A second registration of the same table fails.
    EditorActionHost host;
    EXPECT_FALSE(registerEditorActions(registry, host));
}

TEST(EditorFindBar, EnterUpDownAndRiderShortcutsInFindBarNeverMutateSelectedEditorText) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("alpha beta\nalpha gamma\nalpha delta\n");
    // Select "alpha" on line 1 (bytes 0..5) and open Find (Ctrl+F).
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_Right, Qt::ShiftModifier);
    h.press(Qt::Key_F, Qt::ControlModifier);

    ASSERT_NE(h.view->findBar(), nullptr);
    EditorFindBar& bar = *h.view->findBar();
    EXPECT_TRUE(h.view->findBarVisible());
    QLineEdit* findField = bar.findField();
    ASSERT_NE(findField, nullptr);
    EXPECT_EQ(findField->text(), QStringLiteral("alpha"));
    ASSERT_EQ(h.view->searchMatches().size(), 3U);
    EXPECT_EQ(bar.matchText(), QStringLiteral("1/3"));

    // Pressing Enter in findField (including ShortcutOverride + KeyPress) steps to match 2/3 and NEVER replaces "alpha"
    // with a newline!
    {
        QKeyEvent overrideDown(QEvent::ShortcutOverride, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(findField, &overrideDown);
        EXPECT_TRUE(overrideDown.isAccepted());
        sendKey(findField, Qt::Key_Return);
    }
    EXPECT_EQ(h.text(), "alpha beta\nalpha gamma\nalpha delta\n");
    EXPECT_EQ(bar.matchText(), QStringLiteral("2/3"));

    // Pressing Down and Up in findField steps matches without moving the editor caret onto another line or mutating
    // text.
    sendKey(findField, Qt::Key_Down);
    EXPECT_EQ(bar.matchText(), QStringLiteral("3/3"));
    sendKey(findField, Qt::Key_Up);
    EXPECT_EQ(bar.matchText(), QStringLiteral("2/3"));
    EXPECT_EQ(h.text(), "alpha beta\nalpha gamma\nalpha delta\n");

    // Rider toggle shortcuts Alt+C (Case), Alt+W (Words), Alt+X (Regex).
    EXPECT_FALSE(bar.options().caseSensitive);
    sendKey(findField, Qt::Key_C, Qt::AltModifier);
    EXPECT_TRUE(bar.options().caseSensitive);
    sendKey(findField, Qt::Key_W, Qt::AltModifier);
    EXPECT_TRUE(bar.options().wholeWord);
    sendKey(findField, Qt::Key_X, Qt::AltModifier);
    EXPECT_TRUE(bar.options().regex);

    // Ctrl+R inside Find bar switches to Replace mode and focuses the Replace field.
    sendKey(findField, Qt::Key_R, Qt::ControlModifier);
    EXPECT_TRUE(bar.replaceVisible());
    EXPECT_EQ(h.text(), "alpha beta\nalpha gamma\nalpha delta\n");
}

TEST(EditorSelection, ExtendAndShrinkSelectionFollowStructuralAndSyntaxHierarchy) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("int f() {\n    return (foo + \"hello world\");\n}\n", cpp());
    h.settle();
    const std::string doc = h.text();
    const std::size_t helloPos = doc.find("hello");
    ASSERT_NE(helloPos, std::string::npos);

    // Place caret inside "hello"
    h.view->setState(text::EditorState::caretAt(ByteOffset{helloPos + 2}));
    const ByteOffset initialCaret{helloPos + 2};

    // 1st Ctrl+W: selects word "hello"
    h.press(Qt::Key_W, Qt::ControlModifier);
    const text::ByteRange wordRange = h.view->editor().state().primary().range();
    EXPECT_EQ(h.document.text().text(wordRange), "hello");

    // 2nd Ctrl+W: selects interior of string "hello world"
    h.press(Qt::Key_W, Qt::ControlModifier);
    const text::ByteRange innerQuoteRange = h.view->editor().state().primary().range();
    EXPECT_EQ(h.document.text().text(innerQuoteRange), "hello world");

    // 3rd Ctrl+W: selects quoted string "\"hello world\""
    h.press(Qt::Key_W, Qt::ControlModifier);
    const text::ByteRange outerQuoteRange = h.view->editor().state().primary().range();
    EXPECT_EQ(h.document.text().text(outerQuoteRange), "\"hello world\"");

    // 4th Ctrl+W: selects interior of parentheses "foo + \"hello world\""
    h.press(Qt::Key_W, Qt::ControlModifier);
    const text::ByteRange innerParenRange = h.view->editor().state().primary().range();
    EXPECT_EQ(h.document.text().text(innerParenRange), "foo + \"hello world\"");

    // 5th Ctrl+W: selects parenthesized expression "(foo + \"hello world\")"
    h.press(Qt::Key_W, Qt::ControlModifier);
    const text::ByteRange outerParenRange = h.view->editor().state().primary().range();
    EXPECT_EQ(h.document.text().text(outerParenRange), "(foo + \"hello world\")");

    // Shrink selection (Ctrl+Shift+W) unwinds the exact history stack back to the caret.
    h.press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), innerParenRange);
    h.press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), outerQuoteRange);
    h.press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), innerQuoteRange);
    h.press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().primary().range(), wordRange);
    h.press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(h.view->editor().state().primary().empty());
    EXPECT_EQ(h.caret(), initialCaret.value());
}

TEST(EditorOccurrences, SelectNextUnselectAndSelectAllOccurrences) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("alpha beta alpha gamma alpha");
    h.view->setState(text::EditorState::caretAt(ByteOffset{1}));

    // 1st Alt+J selects word "alpha" at [0..5]
    h.press(Qt::Key_J, Qt::AltModifier);
    EXPECT_EQ(h.view->editor().state().selections.size(), 1U);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{5}}));

    // 2nd Alt+J adds second "alpha" at [11..16]
    h.press(Qt::Key_J, Qt::AltModifier);
    EXPECT_EQ(h.view->editor().state().selections.size(), 2U);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{11}, ByteOffset{16}}));

    // Alt+Shift+J unselects the second occurrence, leaving the first
    h.press(Qt::Key_J, Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().selections.size(), 1U);
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{0}, ByteOffset{5}}));

    // Ctrl+Alt+Shift+J selects all 3 occurrences of "alpha"
    h.press(Qt::Key_J, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.view->editor().state().selections.size(), 3U);
}

TEST(EditorEdit, ToggleCaseConvertsSelectionAndWordUnderCaret) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("hello World");
    // Caret on "hello" with no selection toggles "hello" -> "HELLO"
    h.view->setState(text::EditorState::caretAt(ByteOffset{2}));
    h.press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "HELLO World");
    // Second toggle converts "HELLO" -> "hello"
    h.press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "hello World");

    // Selection over mixed-case "World" converts to "WORLD", then "world"
    const text::Selection sel{ByteOffset{6}, ByteOffset{11}};
    h.view->setState(text::EditorState{.selections = text::SelectionSet::fromSelections({&sel, 1}, 0), .carets = {}});
    h.press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "hello WORLD");
    EXPECT_EQ(h.view->editor().state().primary().range(), (text::ByteRange{ByteOffset{6}, ByteOffset{11}}));
    h.press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(h.text(), "hello world");
}

TEST(EditorColumnMode, ColumnModeClonesCaretsAndSupportsRectangularDrag) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("abcd\nefgh\nijkl\n");
    EXPECT_FALSE(h.view->columnMode());
    EXPECT_FALSE(h.view->status().columnMode);

    // Toggle column mode via action (Alt+Shift+Insert)
    EXPECT_TRUE(h.view->perform(EditorAction::ToggleColumnMode));
    EXPECT_TRUE(h.view->columnMode());
    EXPECT_TRUE(h.view->status().columnMode);

    // Place caret at column 1 of line 0 and invoke MoveDownWithSelection twice to clone carets down
    h.view->setState(text::EditorState::caretAt(ByteOffset{1}));
    EXPECT_TRUE(h.view->perform(EditorAction::MoveDownWithSelection));
    EXPECT_EQ(h.view->editor().state().selections.size(), 2U);
    EXPECT_TRUE(h.view->perform(EditorAction::MoveDownWithSelection));
    ASSERT_EQ(h.view->editor().state().selections.size(), 3U);
    EXPECT_EQ(h.view->editor().state().selections.all()[0].head, ByteOffset{1});
    EXPECT_EQ(h.view->editor().state().selections.all()[1].head, ByteOffset{6});
    EXPECT_EQ(h.view->editor().state().selections.all()[2].head, ByteOffset{11});

    // Alt+Drag rectangular selection from row 0 col 1 to row 2 col 3
    h.view->setColumnMode(false);
    const QRectF startRect = h.view->globalCaretRect(ByteOffset{1});
    (void)startRect;
    const double originX = h.view->buildDisplayList().textArea.left() + 6.0;
    const double cellW = static_cast<double>(h.view->shaper().cellWidth()) / 64.0;
    const double lineH = h.view->lineHeight();
    const QPointF p0(originX + 1.0 * cellW, 0.5 * lineH);
    const QPointF p1(originX + 3.0 * cellW, 2.5 * lineH);

    QMouseEvent press(QEvent::MouseButtonPress, p0, h.view->viewport()->mapToGlobal(p0.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::AltModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &press);
    QMouseEvent move(QEvent::MouseMove, p1, h.view->viewport()->mapToGlobal(p1.toPoint()), Qt::NoButton, Qt::LeftButton,
                     Qt::AltModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &move);
    QMouseEvent release(QEvent::MouseButtonRelease, p1, h.view->viewport()->mapToGlobal(p1.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::AltModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &release);

    ASSERT_EQ(h.view->editor().state().selections.size(), 3U);
    EXPECT_EQ(h.view->editor().state().selections.all()[0].range(), (text::ByteRange{ByteOffset{1}, ByteOffset{3}}));
    EXPECT_EQ(h.view->editor().state().selections.all()[1].range(), (text::ByteRange{ByteOffset{6}, ByteOffset{8}}));
    EXPECT_EQ(h.view->editor().state().selections.all()[2].range(), (text::ByteRange{ByteOffset{11}, ByteOffset{13}}));
}

TEST(EditorViewFeatures, CtrlWheelZoomIndentGuidesWhitespaceAndBookmarks) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("    \tint x = 1;\n        return x;\n");

    // 1. Ctrl+Wheel font zoom and resetFontZoom
    const double initialSize = h.view->effectiveFontSize();
    {
        QWheelEvent wheelUp(QPointF(20.0, 20.0), QPointF(20.0, 20.0), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                            Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(h.view->viewport(), &wheelUp);
    }
    EXPECT_DOUBLE_EQ(h.view->effectiveFontSize(), initialSize + 2.0);
    h.view->resetFontZoom();
    EXPECT_DOUBLE_EQ(h.view->effectiveFontSize(), initialSize);

    // 2. Indent guides and visible whitespace in buildDisplayList
    EditorViewSettings settings = h.view->settings();
    settings.indentGuides = true;
    settings.showWhitespace = "all";
    settings.showRightMargin = false;
    h.view->applySettings(settings);

    const EditorDisplayList list = h.view->buildDisplayList();
    // Line 0 has 8 cols of indent (2 guides), line 1 has 8 cols of indent (2 guides)
    EXPECT_GE(list.underlines.size(), 4U);
    // Visible whitespace emits '·' (U+00B7) for spaces and '→' (U+2192) for tabs
    EXPECT_NE(list.findText(QString(QChar(0x00B7))), nullptr);
    EXPECT_NE(list.findText(QString(QChar(0x2192))), nullptr);

    // 3. Bookmarks in gutter markers
    h.view->setBookmarks({LineIndex{1}});
    ASSERT_EQ(h.view->bookmarks().size(), 1U);
    EXPECT_EQ(h.view->bookmarks()[0], LineIndex{1});
    const EditorDisplayList withBookmarks = h.view->buildDisplayList();
    ASSERT_EQ(withBookmarks.markers.size(), 1U);
    EXPECT_EQ(withBookmarks.markers[0].line, LineIndex{1});
    EXPECT_EQ(withBookmarks.markers[0].severity, MarkerSeverity::Bookmark);
    EXPECT_EQ(withBookmarks.markers[0].color, h.view->colors().markerBookmark);
}

} // namespace
} // namespace frost::ui
