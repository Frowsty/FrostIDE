// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of Find and Replace in Files in the shell (frost/ui/find; ADR 0045), offscreen,
// over a scratch tree on the real file system and a two-worker pool: the popup lists the results
// with their highlight and preview and Enter opens the selected one; replace mode checks every
// match and an unchecked one stays; Open in Find Window groups the results by file with counts and
// the occurrence steps walk them; the actions carry the IntelliJ shortcuts and the selection
// pre-fills the query; Replace All posts "N replacements in M files, K skipped".

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/input/keymap.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_item_roles.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/find/find_session.hpp"
#include "frost/ui/find/find_tool_window.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "search/search_test_util.hpp"
#include "shell/shell_rig.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui::test {
namespace {

using search::test::Tree;

void sendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
}

// The documents, the pool and a scratch tree: a.cpp with two matches, b.h with one, c.txt none.
class FindUi : public ::testing::Test {
protected:
    FindUi() {
        a = tree.write("src/a.cpp", "int needle = 1;\n// context\nreturn needle;\n");
        b = tree.write("include/b.h", "#pragma once\n    void needle();\n");
        tree.write("c.txt", "nothing here\n");
        document::SaveManagerServices services;
        services.fs = &io::realFileSystem();
        services.clock = &systemClock();
        document::SaveManagerOptions options;
        options.load = search::test::loadOptions();
        document::SavePolicies policies;
        policies.hotExit = false;
        manager = std::make_unique<document::SaveManager>(services, options, policies);
        sched::ThreadPoolOptions poolOptions;
        poolOptions.workerCount = 2;
        pool = std::make_unique<sched::ThreadPool>(poolOptions);
    }

    [[nodiscard]] FindSessionServices sessionServices() {
        return FindSessionServices{pool.get(), manager.get(), {}, {}};
    }

    [[nodiscard]] FindSettings directorySettings(std::string pattern) const {
        FindSettings settings;
        settings.query.pattern = std::move(pattern);
        settings.scope.kind = search::ScopeKind::Directory;
        settings.scope.directory = tree.root();
        return settings;
    }

    [[nodiscard]] FindFeatureServices featureServices() {
        FindFeatureServices services;
        services.scheduler = pool.get();
        services.documents = manager.get();
        services.defaultDirectory = [this] { return tree.root(); };
        services.selectedText = [this] { return selection; };
        return services;
    }

    // Declared first, destroyed last: sessions join their searches before the pool goes.
    Tree tree{"find-ui"};
    std::unique_ptr<sched::ThreadPool> pool;
    std::unique_ptr<document::SaveManager> manager;
    std::string a;
    std::string b;
    QString selection;
};

TEST_F(FindUi, PopupShowsResultsAndEnterOpensTheSelectedMatch) {
    QWidget host;
    FindPopup popup(&host, sessionServices(), directorySettings("needle"), false);
    popup.setDeleteOnClose(false);
    EXPECT_FALSE(popup.scopeButton(search::ScopeKind::Solution)->isEnabled()); // No solution.
    EXPECT_TRUE(popup.scopeButton(search::ScopeKind::Directory)->isChecked());
    EXPECT_FALSE(popup.replacementField()->isVisible());
    popup.searchNow();
    popup.session()->wait();

    QListWidget* results = popup.resultList();
    ASSERT_EQ(results->count(), 3);
    EXPECT_EQ(popup.statusLabel()->text(), QStringLiteral("3 matches in 2 files"));
    // Rows: the line without its indentation, the match's range in it, and "file line".
    std::vector<QString> details;
    for (int row = 0; row < results->count(); ++row) {
        details.push_back(results->item(row)->data(kFindDetailRole).toString());
    }
    EXPECT_NE(std::ranges::find(details, QStringLiteral("a.cpp 1")), details.end());
    EXPECT_NE(std::ranges::find(details, QStringLiteral("a.cpp 3")), details.end());
    EXPECT_NE(std::ranges::find(details, QStringLiteral("b.h 2")), details.end());
    for (int row = 0; row < results->count(); ++row) {
        const QListWidgetItem* item = results->item(row);
        if (item->data(kFindDetailRole).toString() == QStringLiteral("b.h 2")) {
            EXPECT_EQ(item->text(), QStringLiteral("void needle();"));
            EXPECT_EQ(item->data(kFindHighlightStartRole).toInt(), 5);
            EXPECT_EQ(item->data(kFindHighlightLengthRole).toInt(), 6);
        }
    }

    // The first row is selected and previewed with its context.
    EXPECT_EQ(results->currentRow(), 0);
    const auto first = popup.currentMatch();
    ASSERT_TRUE(first);
    const FindLocation firstLocation = popup.session()->location(first->first, first->second);
    EXPECT_TRUE(popup.previewTitle()->text().endsWith(QStringLiteral(":%1").arg(firstLocation.line + 1)));
    EXPECT_FALSE(popup.preview()->toPlainText().isEmpty());
    EXPECT_EQ(popup.preview()->extraSelections().size(), 1);

    // Down in the query field moves the selection; Enter opens it and closes the popup.
    std::optional<FindLocation> opened;
    QObject::connect(&popup, &FindPopup::locationActivated,
                     [&opened](const FindLocation& location) { opened = location; });
    sendKey(popup.queryField(), Qt::Key_Down);
    ASSERT_EQ(results->currentRow(), 1);
    const auto second = popup.currentMatch();
    ASSERT_TRUE(second);
    const FindLocation expected = popup.session()->location(second->first, second->second);
    sendKey(popup.queryField(), Qt::Key_Return);
    ASSERT_TRUE(opened);
    EXPECT_EQ(*opened, expected);
    EXPECT_EQ(expected.length, 6U);
    EXPECT_FALSE(popup.isOpen());
}

TEST_F(FindUi, ReplaceModeChecksEveryMatchAndLeavesUncheckedOnesOut) {
    QWidget host;
    FindSettings settings = directorySettings("needle");
    settings.replaceMode = true;
    settings.replacement = "pin";
    FindPopup popup(&host, sessionServices(), settings, false);
    popup.setDeleteOnClose(false);
    popup.searchNow();
    popup.session()->wait();
    QListWidget* results = popup.resultList();
    ASSERT_EQ(results->count(), 3);
    for (int row = 0; row < results->count(); ++row) {
        EXPECT_EQ(results->item(row)->checkState(), Qt::Checked);
    }
    EXPECT_TRUE(popup.replaceAllButton()->isEnabled());
    results->item(1)->setCheckState(Qt::Unchecked);
    EXPECT_EQ(popup.session()->includedCount(), 2U);
    const search::ReplaceRequest request = popup.session()->replaceRequest();
    std::size_t ranges = 0;
    for (const search::ReplaceFilePlan& plan : request.files) {
        ranges += plan.ranges.size();
    }
    EXPECT_EQ(ranges, 2U);
    EXPECT_EQ(request.replacement, "pin");
}

TEST_F(FindUi, ActionsCarryTheIntelliJShortcutsAndTheSelectionFillsTheQuery) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    FindFeature feature(window, featureServices());
    ASSERT_TRUE(feature.install());
    const auto shortcut = [&rig](std::string_view id) {
        const std::optional<input::ActionId> action = rig.registry.find(id);
        if (!action) {
            return std::string("<unregistered>");
        }
        const std::optional<input::Sequence> sequence = rig.keymap->primaryShortcut(*action);
        return sequence ? input::toDisplayText(*sequence, rig.keymap->layout(), rig.keymap->platform()) : std::string();
    };
    EXPECT_EQ(shortcut(find_actions::kFindInFiles), "Ctrl+Shift+F");
    EXPECT_EQ(shortcut(find_actions::kReplaceInFiles), "Ctrl+Shift+R");
    EXPECT_EQ(shortcut(find_actions::kNextOccurrence), "Ctrl+Alt+Down");
    EXPECT_EQ(shortcut(find_actions::kPreviousOccurrence), "Ctrl+Alt+Up");
    EXPECT_EQ(shortcut(find_actions::kFindWindow), "Alt+3");
    EXPECT_TRUE(window.toolWindows().contains(find_actions::kFindToolWindow));
    // Installing twice registers nothing twice.
    EXPECT_TRUE(feature.install());

    selection = QStringLiteral("needle");
    EXPECT_EQ(window.invoke(find_actions::kReplaceInFiles), input::InvokeResult::Ran);
    FindPopup* popup = feature.popup();
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(popup->replaceMode());
    EXPECT_EQ(popup->queryField()->text(), QStringLiteral("needle"));
    // No solution: the Directory scope at the default directory.
    EXPECT_TRUE(popup->scopeButton(search::ScopeKind::Directory)->isChecked());
    EXPECT_EQ(popup->directoryField()->text(), QString::fromStdString(tree.root()));
    popup->closePopup(Popup::CloseReason::Cancelled);
    ASSERT_TRUE(feature.lastSettings());
    EXPECT_TRUE(feature.lastSettings()->replaceMode);
    // A multi-line selection keeps the last query.
    selection = QStringLiteral("two\nlines");
    FindPopup* again = feature.showPopup(false);
    EXPECT_EQ(again->queryField()->text(), QStringLiteral("needle"));
    again->closePopup(Popup::CloseReason::Cancelled);
}

TEST_F(FindUi, OpenInFindWindowGroupsByFileAndStepsThroughOccurrences) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    std::vector<FindLocation> opened;
    FindFeatureServices services = featureServices();
    services.openLocation = [&opened](const FindLocation& location) { opened.push_back(location); };
    FindFeature feature(window, std::move(services));
    ASSERT_TRUE(feature.install());
    selection = QStringLiteral("needle");
    FindPopup* popup = feature.showPopup(false);
    ASSERT_NE(popup, nullptr);
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->resultList()->count(), 3);

    FindToolWindow* findWindow = feature.openInFindWindow(*popup);
    ASSERT_NE(findWindow, nullptr);
    EXPECT_EQ(feature.toolWindow(), findWindow);
    EXPECT_TRUE(window.toolWindows().isVisible(find_actions::kFindToolWindow));
    EXPECT_EQ(feature.popup() == nullptr || !feature.popup()->isOpen(), true);
    ASSERT_NE(findWindow->session(), nullptr);

    // One row per file with its count, the matches below it.
    QTreeWidget* tree = findWindow->tree();
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    int total = 0;
    for (int f = 0; f < tree->topLevelItemCount(); ++f) {
        const QTreeWidgetItem* file = tree->topLevelItem(f);
        const int children = file->childCount();
        total += children;
        if (file->text(0) == QStringLiteral("a.cpp")) {
            EXPECT_EQ(children, 2);
            EXPECT_TRUE(file->data(0, kFindDetailRole).toString().endsWith(QStringLiteral("2 matches")));
        } else {
            EXPECT_EQ(file->text(0), QStringLiteral("b.h"));
            EXPECT_EQ(children, 1);
            EXPECT_TRUE(file->data(0, kFindDetailRole).toString().endsWith(QStringLiteral("1 match")));
        }
    }
    EXPECT_EQ(total, 3);
    EXPECT_TRUE(findWindow->statusLabel()->text().contains(QStringLiteral("3 matches in 2 files")));

    // Next Occurrence walks the matches in tree order and wraps; Previous goes back.
    EXPECT_EQ(window.invoke(find_actions::kNextOccurrence), input::InvokeResult::Ran);
    EXPECT_EQ(window.invoke(find_actions::kNextOccurrence), input::InvokeResult::Ran);
    EXPECT_EQ(window.invoke(find_actions::kNextOccurrence), input::InvokeResult::Ran);
    EXPECT_EQ(window.invoke(find_actions::kNextOccurrence), input::InvokeResult::Ran);
    ASSERT_EQ(opened.size(), 4U);
    EXPECT_EQ(opened[3], opened[0]);
    EXPECT_NE(opened[1], opened[0]);
    EXPECT_NE(opened[2], opened[1]);
    EXPECT_EQ(window.invoke(find_actions::kPreviousOccurrence), input::InvokeResult::Ran);
    ASSERT_EQ(opened.size(), 5U);
    EXPECT_EQ(opened[4], opened[2]);

    // Rerun searches again into the same window.
    findWindow->rerunButton()->click();
    findWindow->session()->wait();
    EXPECT_EQ(tree->topLevelItemCount(), 2);
}

TEST_F(FindUi, ReplaceAllPostsTheSummaryAndKeepsUncheckedMatches) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    FindFeature feature(window, featureServices());
    ASSERT_TRUE(feature.install());
    selection = QStringLiteral("needle");
    FindPopup* popup = feature.showPopup(true);
    ASSERT_NE(popup, nullptr);
    popup->replacementField()->setText(QStringLiteral("pin"));
    popup->searchNow();
    popup->session()->wait();
    QListWidget* results = popup->resultList();
    ASSERT_EQ(results->count(), 3);
    // Leave b.h's match out.
    for (int row = 0; row < results->count(); ++row) {
        if (results->item(row)->data(kFindDetailRole).toString().startsWith(QStringLiteral("b.h"))) {
            results->item(row)->setCheckState(Qt::Unchecked);
        }
    }
    auto summary = feature.replace(*popup, true);
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_EQ(summary->replacements, 2U);
    EXPECT_EQ(tree.read(a), "int pin = 1;\n// context\nreturn pin;\n");
    EXPECT_EQ(tree.read(b), "#pragma once\n    void needle();\n");
    ASSERT_FALSE(window.notifications().log().empty());
    const Notification& posted = window.notifications().log().back().notification;
    EXPECT_EQ(posted.text, QStringLiteral("2 replacements in 1 files, 0 skipped"));
    EXPECT_EQ(posted.kind, NotificationKind::Info);
}

TEST_F(FindUi, WithoutAnOpenLocationCallbackTheMatchOpensInATab) {
    ShellRig rig;
    ShellWindow window(rig.environment());
    FindFeature feature(window, featureServices());
    ASSERT_TRUE(feature.install());
    selection = QStringLiteral("needle");
    FindPopup* popup = feature.showPopup(false);
    popup->fileMaskToggle()->setChecked(true);
    popup->fileMaskField()->setText(QStringLiteral("*.h"));
    popup->searchNow();
    popup->session()->wait();
    ASSERT_EQ(popup->resultList()->count(), 1);
    ASSERT_TRUE(popup->activateCurrent());
    ASSERT_NE(rig.editors.last, nullptr);
    EXPECT_EQ(rig.editors.last->path(), b);
    ASSERT_EQ(rig.editors.last->gotoCalls.size(), 1U);
    EXPECT_EQ(rig.editors.last->gotoCalls[0], (std::pair<int, int>{2, 10}));
}

} // namespace
} // namespace frost::ui::test
