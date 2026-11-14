// SPDX-License-Identifier: Apache-2.0
//
// The language-feature widgets offscreen (frost/ui/lang/lang_widgets.hpp): the completion
// lookup shows and filters its items, moves with Up/Down, commits on Enter and Tab and cancels
// on Esc; parameter info bolds the active parameter; quick doc renders Markdown; the usages panel
// groups by file and opens a double-clicked usage; the symbol popup filters its tree and
// activates; the rename field and the choice list accept and cancel.

#include "frost/ui/lang/lang_widgets.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

namespace fl = frost::lang;
using frost::ui::lang::ChoicePopup;
using frost::ui::lang::CompletionPopup;
using frost::ui::lang::FindUsagesPanel;
using frost::ui::lang::RenameInput;
using frost::ui::lang::SymbolPopup;

void press(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &event);
}

fl::CompletionItem item(const std::string& label, fl::CompletionItemKind kind, std::string detail = {},
                        std::string documentation = {}) {
    fl::CompletionItem out;
    out.label = label;
    out.filterText = label;
    out.sortText = label;
    out.insertText = label;
    out.kind = kind;
    out.detail = std::move(detail);
    out.documentation = fl::Markup{.text = std::move(documentation), .markdown = true};
    return out;
}

TEST(CompletionPopupWidget, EnterCommitsActualServerPreselectWithinBestMatchClass) {
    CompletionPopup popup;
    auto first = item("alpha", fl::CompletionItemKind::Variable);
    auto preferred = item("alpine", fl::CompletionItemKind::Variable); preferred.preselect = true;
    fl::CompletionList list; list.items = {first, preferred};
    popup.setList(std::move(list)); popup.setPrefix("al");
    std::optional<std::size_t> committed;
    QObject::connect(&popup, &CompletionPopup::commitRequested, [&](std::size_t index, bool) { committed = index; });
    EXPECT_TRUE(popup.handleKey(Qt::Key_Return, Qt::NoModifier));
    ASSERT_TRUE(committed); EXPECT_EQ(*committed, 1U);
}

TEST(CompletionPopupWidget, ShowsFiltersMovesAndCommits) {
    CompletionPopup popup;
    fl::CompletionList list;
    list.items = {item("value", fl::CompletionItemKind::Variable, "int", "The **value**."),
                  item("valueOf", fl::CompletionItemKind::Function, "int (int)"),
                  item("other", fl::CompletionItemKind::Class)};
    std::vector<std::pair<std::size_t, bool>> commits;
    int cancels = 0;
    QObject::connect(&popup, &CompletionPopup::commitRequested,
                     [&](std::size_t index, bool replace) { commits.emplace_back(index, replace); });
    QObject::connect(&popup, &CompletionPopup::cancelled, [&] { ++cancels; });
    popup.setList(std::move(list));
    popup.setPrefix("val");
    popup.showAt(QRect(100, 100, 2, 16));
    ASSERT_TRUE(popup.isVisible());
    EXPECT_EQ(popup.rowCount(), 2);
    EXPECT_TRUE(popup.documentationText().contains(QStringLiteral("The value.")));
    EXPECT_FALSE(popup.documentationText().contains(QStringLiteral("**")));

    // Down selects valueOf; Enter commits it (item index 1 in the list as given).
    EXPECT_TRUE(popup.handleKey(Qt::Key_Down, Qt::NoModifier));
    press(&popup, Qt::Key_Return);
    ASSERT_EQ(commits.size(), 1);
    EXPECT_EQ(commits[0], (std::pair<std::size_t, bool>{1, false}));
    // Up wraps back to value; Tab commits replacing the word.
    EXPECT_TRUE(popup.handleKey(Qt::Key_Up, Qt::NoModifier));
    EXPECT_TRUE(popup.handleKey(Qt::Key_Tab, Qt::NoModifier));
    ASSERT_EQ(commits.size(), 2);
    EXPECT_EQ(commits[1], (std::pair<std::size_t, bool>{0, true}));
    // Other keys are the editor's.
    EXPECT_FALSE(popup.handleKey(Qt::Key_A, Qt::NoModifier));
    EXPECT_FALSE(popup.handleKey(Qt::Key_Space, Qt::ControlModifier));

    press(&popup, Qt::Key_Escape);
    EXPECT_EQ(cancels, 1);
    EXPECT_FALSE(popup.isVisible());
    EXPECT_FALSE(popup.handleKey(Qt::Key_Return, Qt::NoModifier)); // Hidden: Enter is a newline.
}

fl::ParameterInfo parameter(std::string label) {
    fl::ParameterInfo out;
    out.label = std::move(label);
    return out;
}

TEST(HintPopups, ParameterInfoBoldsTheActiveParameterAndQuickDocRendersMarkdown) {
    frost::ui::lang::ParameterInfoPopup parameters;
    fl::SignatureHelp help;
    help.signatures.push_back(fl::SignatureInfo{.label = "int add(int a, int b)",
                                                .documentation = {},
                                                .parameters = {parameter("int a"), parameter("int b")},
                                                .activeParameter = std::nullopt});
    help.signatures.push_back(fl::SignatureInfo{.label = "int add(int a)",
                                                .documentation = {},
                                                .parameters = {parameter("int a")},
                                                .activeParameter = std::nullopt});
    help.activeParameter = 1;
    parameters.setHelp(help);
    EXPECT_TRUE(parameters.html().contains(QStringLiteral("<b>int b</b>")));
    EXPECT_TRUE(parameters.html().contains(QStringLiteral("color:gray"))); // The other overload.
    parameters.showAt(QRect(100, 300, 2, 16), true);
    EXPECT_TRUE(parameters.isVisible());
    EXPECT_FALSE(parameters.isActiveWindow());

    frost::ui::lang::QuickDocPopup doc;
    doc.setMarkup(fl::Markup{.text = "### function `add`\n\nAdds **two** numbers.", .markdown = true});
    EXPECT_TRUE(doc.plainText().contains(QStringLiteral("Adds two numbers.")));
    EXPECT_TRUE(doc.plainText().contains(QStringLiteral("function add")));
    EXPECT_TRUE(frost::ui::lang::markupToHtml(fl::Markup{.text = "a < b", .markdown = false})
                    .contains(QStringLiteral("a &lt; b")));
    EXPECT_FALSE(frost::ui::lang::kindIcon(fl::CompletionItemKind::Class).isNull());
    EXPECT_FALSE(frost::ui::lang::kindIcon(fl::SymbolKind::Namespace).isNull());
}

TEST(FindUsagesPanelWidget, GroupsByFileAndOpensUsages) {
    FindUsagesPanel panel;
    const auto usage = [](std::string path, std::uint32_t line, std::string text) {
        fl::Usage out;
        out.location =
            fl::Location{.path = std::move(path), .uri = {}, .range = fl::LspRange{{line - 1, 4}, {line - 1, 9}}};
        out.line = line;
        out.column = 5;
        out.lineText = std::move(text);
        return out;
    };
    std::vector<fl::UsageGroup> groups{
        {.path = "/w/a.cpp", .usages = {usage("/w/a.cpp", 1, "int value;"), usage("/w/a.cpp", 7, "value = 2;")}},
        {.path = "/w/b.cpp", .usages = {usage("/w/b.cpp", 3, "return value;")}}};
    panel.setUsages(QStringLiteral("Usages of 'value'"), std::move(groups));
    EXPECT_EQ(panel.usageCount(), 3u);
    EXPECT_EQ(panel.title(), QStringLiteral("Usages of 'value': 3 usages"));
    ASSERT_EQ(panel.tree()->topLevelItemCount(), 2);
    EXPECT_EQ(panel.tree()->topLevelItem(0)->text(0), QStringLiteral("a.cpp  2 usages"));
    EXPECT_EQ(panel.tree()->topLevelItem(0)->child(1)->text(0), QStringLiteral("7  value = 2;"));

    std::vector<std::pair<QString, fl::LspPosition>> opened;
    QObject::connect(&panel, &FindUsagesPanel::openRequested,
                     [&](const QString& path, fl::LspPosition position) { opened.emplace_back(path, position); });
    emit panel.tree()->itemActivated(panel.tree()->topLevelItem(1)->child(0), 0); // A double-click.
    ASSERT_EQ(opened.size(), 1);
    EXPECT_EQ(opened[0].first, QStringLiteral("/w/b.cpp"));
    EXPECT_EQ(opened[0].second.line, 2u);
    emit panel.tree()->itemActivated(panel.tree()->topLevelItem(0), 0); // A file row opens nothing.
    EXPECT_EQ(opened.size(), 1);
}

TEST(SymbolPopupWidget, FiltersTheStructureAndActivates) {
    SymbolPopup popup;
    const auto symbol = [](std::string name, fl::SymbolKind kind, std::uint32_t line,
                           std::vector<fl::DocumentSymbol> children = {}) {
        fl::DocumentSymbol out;
        out.name = std::move(name);
        out.kind = kind;
        out.range = fl::LspRange{{line, 0}, {line, 40}};
        out.selectionRange = fl::LspRange{{line, 6}, {line, 10}};
        out.children = std::move(children);
        return out;
    };
    popup.setStructure(
        QStringLiteral("Structure"), "/w/a.cpp",
        {symbol("Widget", fl::SymbolKind::Class, 1,
                {symbol("paint", fl::SymbolKind::Method, 2), symbol("resize", fl::SymbolKind::Method, 3)}),
         symbol("main", fl::SymbolKind::Function, 9)});
    popup.popup(nullptr);
    EXPECT_TRUE(popup.isVisible());
    EXPECT_EQ(popup.visibleRowCount(), 4);
    popup.setQuery(QStringLiteral("pai"));
    EXPECT_EQ(popup.visibleRowCount(), 2); // Widget stays as paint's context.

    std::vector<std::pair<QString, fl::LspPosition>> activated;
    QObject::connect(&popup, &SymbolPopup::activated,
                     [&](const QString& path, fl::LspPosition position) { activated.emplace_back(path, position); });
    press(popup.findChild<QLineEdit*>(QStringLiteral("symbolSearch")), Qt::Key_Return);
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated[0].first, QStringLiteral("/w/a.cpp"));
    EXPECT_EQ(activated[0].second, (fl::LspPosition{2, 6}));
    EXPECT_FALSE(popup.isVisible());

    // Go to symbol: rows from the controller; typing asks for new ones.
    int queries = 0;
    QObject::connect(&popup, &SymbolPopup::queryChanged, [&](const QString&) { ++queries; });
    fl::WorkspaceSymbol found;
    found.name = "tinyValue";
    found.containerName = "ns";
    found.kind = fl::SymbolKind::Function;
    found.location = fl::Location{.path = "/w/tiny.h", .uri = {}, .range = fl::LspRange{{4, 4}, {4, 13}}};
    popup.setWorkspaceSymbols(QStringLiteral("Go to Symbol"), {found});
    popup.popup(nullptr);
    popup.setQuery(QStringLiteral("tv"));
    EXPECT_EQ(queries, 1);
    EXPECT_EQ(popup.visibleRowCount(), 1);
    popup.activateCurrent();
    ASSERT_EQ(activated.size(), 2);
    EXPECT_EQ(activated[1].first, QStringLiteral("/w/tiny.h"));
    popup.popup(nullptr);
    press(popup.findChild<QLineEdit*>(QStringLiteral("symbolSearch")), Qt::Key_Escape);
    EXPECT_FALSE(popup.isVisible());
}

TEST(RenameAndChoicePopups, AcceptAndCancel) {
    RenameInput rename;
    std::vector<QString> accepted;
    int renameCancelled = 0;
    QObject::connect(&rename, &RenameInput::accepted, [&](const QString& name) { accepted.push_back(name); });
    QObject::connect(&rename, &RenameInput::cancelled, [&] { ++renameCancelled; });
    rename.start(QStringLiteral("value"), QRect(50, 50, 2, 16));
    EXPECT_TRUE(rename.isVisible());
    EXPECT_EQ(rename.field()->selectedText(), QStringLiteral("value"));
    rename.field()->setText(QStringLiteral("amount"));
    press(rename.field(), Qt::Key_Return);
    ASSERT_EQ(accepted.size(), 1);
    EXPECT_EQ(accepted[0], QStringLiteral("amount"));
    EXPECT_FALSE(rename.isVisible());
    rename.start(QStringLiteral("value"), QRect(50, 50, 2, 16));
    press(rename.field(), Qt::Key_Escape);
    EXPECT_EQ(renameCancelled, 1);
    EXPECT_EQ(accepted.size(), 1);

    ChoicePopup choice;
    std::vector<std::size_t> chosen;
    int choiceCancelled = 0;
    QObject::connect(&choice, &ChoicePopup::chosen, [&](std::size_t index) { chosen.push_back(index); });
    QObject::connect(&choice, &ChoicePopup::cancelled, [&] { ++choiceCancelled; });
    choice.start(QStringLiteral("Quick fixes"), {QStringLiteral("Insert ';'"), QStringLiteral("Add #include")},
                 QRect(50, 50, 2, 16), {true, false});
    EXPECT_EQ(choice.list()->count(), 2);
    EXPECT_TRUE(choice.list()->item(0)->font().bold());
    press(choice.list(), Qt::Key_Down);
    press(choice.list(), Qt::Key_Return);
    ASSERT_EQ(chosen.size(), 1);
    EXPECT_EQ(chosen[0], 1u);
    EXPECT_FALSE(choice.isVisible());
    choice.start(QString(), {QStringLiteral("only")}, QRect(50, 50, 2, 16));
    press(choice.list(), Qt::Key_Escape);
    EXPECT_EQ(choiceCancelled, 1);
    EXPECT_EQ(chosen.size(), 1);
}

} // namespace
