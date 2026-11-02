// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of frost::ui_vcs without git (offscreen): the Commit window's model over a parsed
// status, the side-by-side diff viewer (highlights, F7 / Shift+F7, synchronized scrolling), the VCS
// status colours of every Glacier theme and the colour provider, and the VCS shortcut defaults.

#include "frost/input/key.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/vcs/commit_window.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/git_actions.hpp"
#include "frost/ui/vcs/vcs_colors.hpp"
#include "frost/vcs/vcs_status.hpp"

#include "editor/editor_harness.hpp"
#include "support/appearance_fixture.hpp"
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

#ifdef _WIN32
constexpr std::string_view kWorkTree = "C:/work/repo";
#else
constexpr std::string_view kWorkTree = "/work/repo";
#endif

std::string at(std::string_view relative) { return vcs::absoluteGitPath(kWorkTree, relative); }

vcs::GitStatusEntry entry(vcs::GitEntryKind kind, char x, char y, std::string path, std::string original = {}) {
    vcs::GitStatusEntry result;
    result.kind = kind;
    result.index = x;
    result.worktree = y;
    result.path = std::move(path);
    result.originalPath = std::move(original);
    return result;
}

vcs::GitStatus sampleStatus() {
    vcs::GitStatus status;
    status.branch.head = "main";
    status.branch.oid = "0123456789abcdef0123456789abcdef01234567";
    status.entries = {
        entry(vcs::GitEntryKind::Ordinary, '.', 'M', "src/main.cpp"),
        entry(vcs::GitEntryKind::Untracked, '?', '?', "notes.txt"),
        entry(vcs::GitEntryKind::Ordinary, 'A', '.', "src/new.cpp"),
        entry(vcs::GitEntryKind::RenamedOrCopied, 'R', '.', "lib/renamed.h", "lib/old.h"),
        entry(vcs::GitEntryKind::Unmerged, 'U', 'U', "conflict.txt"),
        entry(vcs::GitEntryKind::Ignored, '!', '!', "build/out.o"),
        entry(vcs::GitEntryKind::Untracked, '?', '?', "scratch/tmp.txt"),
    };
    return status;
}

TEST(CommitChangesModel, GroupsTheChangesAndTheUnversionedFiles) {
    CommitChangesModel model;
    model.setStatus(std::string(kWorkTree), sampleStatus());
    ASSERT_NE(model.changesGroup(), nullptr);
    ASSERT_NE(model.unversionedGroup(), nullptr);
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.changesGroup()->text(), QStringLiteral("Changes"));
    EXPECT_EQ(model.unversionedGroup()->text(), QStringLiteral("Unversioned Files"));
    // Sorted by path; the ignored file is not listed.
    ASSERT_EQ(model.changesGroup()->rowCount(), 4);
    EXPECT_EQ(model.changesGroup()->child(0)->text(), QStringLiteral("conflict.txt"));
    EXPECT_EQ(model.changesGroup()->child(1)->text(), QStringLiteral("renamed.h"));
    EXPECT_EQ(model.changesGroup()->child(1, 1)->text(), QStringLiteral("lib"));
    EXPECT_TRUE(model.changesGroup()->child(1)->toolTip().contains(QStringLiteral("renamed from lib/old.h")));
    EXPECT_EQ(model.changesGroup()->child(1)->data(CommitChangesModel::StatusRole).toInt(),
              static_cast<int>(vcs::VcsFileStatus::Renamed));
    ASSERT_EQ(model.unversionedGroup()->rowCount(), 2);
    EXPECT_EQ(model.itemFor(at("build/out.o")), nullptr);
    EXPECT_EQ(model.allPaths().size(), 6U);

    // Tracked changes start checked, unversioned files unchecked.
    EXPECT_EQ(model.checkedPaths(), (std::vector<std::string>{at("conflict.txt"), at("lib/renamed.h"),
                                                              at("src/main.cpp"), at("src/new.cpp")}));
    EXPECT_EQ(model.changesGroup()->checkState(), Qt::Checked);
    EXPECT_EQ(model.unversionedGroup()->checkState(), Qt::Unchecked);
    // Files are coloured by status.
    EXPECT_EQ(model.itemFor(at("src/new.cpp"))->foreground().color(), *vcsStatusColor(vcs::VcsFileStatus::Added));
}

TEST(CommitChangesModel, KeepsTheUsersChoicesAcrossRefreshesAndChecksWholeGroups) {
    CommitChangesModel model;
    model.setStatus(std::string(kWorkTree), sampleStatus());
    model.setChecked(at("src/main.cpp"), false);
    model.setChecked(at("notes.txt"), true);
    EXPECT_EQ(model.changesGroup()->checkState(), Qt::PartiallyChecked);
    EXPECT_EQ(model.unversionedGroup()->checkState(), Qt::PartiallyChecked);

    // A refresh (a new file appeared, one went) keeps both choices.
    vcs::GitStatus next = sampleStatus();
    next.entries.erase(next.entries.begin() + 4); // conflict.txt was resolved and committed.
    next.entries.push_back(entry(vcs::GitEntryKind::Ordinary, '.', 'D', "gone.txt"));
    model.setStatus(std::string(kWorkTree), next);
    EXPECT_FALSE(model.isChecked(at("src/main.cpp")));
    EXPECT_TRUE(model.isChecked(at("notes.txt")));
    EXPECT_TRUE(model.isChecked(at("gone.txt"))); // A new change starts checked.
    EXPECT_FALSE(model.isChecked(at("scratch/tmp.txt")));
    EXPECT_EQ(model.itemFor(at("conflict.txt")), nullptr);

    // The group's box checks every file of the group, and clears them again.
    model.unversionedGroup()->setCheckState(Qt::Checked);
    EXPECT_TRUE(model.isChecked(at("scratch/tmp.txt")));
    EXPECT_EQ(model.unversionedGroup()->checkState(), Qt::Checked);
    model.changesGroup()->setCheckState(Qt::Unchecked);
    EXPECT_EQ(model.checkedPaths(), (std::vector<std::string>{at("notes.txt"), at("scratch/tmp.txt")}));

    // A clean work tree hides both groups.
    vcs::GitStatus clean;
    clean.branch = next.branch;
    model.setStatus(std::string(kWorkTree), clean);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.changesGroup(), nullptr);
    EXPECT_TRUE(model.checkedPaths().empty());
}

// "line 0\n" ... "line <count-1>\n".
std::string numberedLines(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += std::format("line {}\n", i);
    }
    return text;
}

struct DiffRig {
    DiffRig() {
        std::string right = numberedLines(200);
        // Line 10 changed, line 50 deleted, a line added after line 120.
        right.replace(right.find("line 10\n"), 8, "line ten\n");
        right.erase(right.find("line 50\n"), 8);
        right.insert(right.find("line 121\n"), "added line\n");
        auto diff = vcs::SideBySideDiff::compute(numberedLines(200), right);
        EXPECT_TRUE(diff.has_value());
        viewer.resize(900, 300);
        viewer.show();
        viewer.setDiff(std::move(*diff), QStringLiteral("HEAD"), QStringLiteral("Working copy"));
        QCoreApplication::processEvents();
    }
    [[nodiscard]] int rightLine() const { return viewer.rightPane()->textCursor().blockNumber(); }
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent down(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(viewer.rightPane(), &down);
    }

    DiffViewer viewer;
};

TEST(DiffViewer, ShowsBothVersionsWithTheirHighlights) {
    FROST_REQUIRE_OFFSCREEN();
    DiffRig rig;
    const DiffViewer& viewer = rig.viewer;
    EXPECT_EQ(viewer.leftTitle(), QStringLiteral("HEAD"));
    EXPECT_EQ(viewer.rightTitle(), QStringLiteral("Working copy"));
    EXPECT_TRUE(viewer.leftPane()->isReadOnly());
    EXPECT_TRUE(viewer.rightPane()->isReadOnly());
    EXPECT_EQ(viewer.leftPane()->document()->blockCount(), 200);
    EXPECT_EQ(viewer.rightPane()->document()->blockCount(), 200);
    EXPECT_EQ(viewer.leftPane()->document()->findBlockByNumber(10).text(), QStringLiteral("line 10"));
    EXPECT_EQ(viewer.rightPane()->document()->findBlockByNumber(10).text(), QStringLiteral("line ten"));

    ASSERT_EQ(viewer.diff().blocks().size(), 3U);
    EXPECT_EQ(viewer.diff().blocks()[0].kind(), vcs::LineChangeKind::Modified);
    EXPECT_EQ(viewer.diff().blocks()[1].kind(), vcs::LineChangeKind::Deleted);
    EXPECT_EQ(viewer.diff().blocks()[2].kind(), vcs::LineChangeKind::Added);
    // Left: the changed and the deleted line, plus the changed word; right: the changed and the
    // added line, plus the changed word.
    EXPECT_EQ(viewer.highlightCount(vcs::DiffSide::Left), 3);
    EXPECT_EQ(viewer.highlightCount(vcs::DiffSide::Right), 3);
    const QList<QTextEdit::ExtraSelection> right = viewer.rightPane()->extraSelections();
    bool word = false;
    for (const QTextEdit::ExtraSelection& selection : right) {
        word = word || selection.cursor.selectedText() == QStringLiteral("ten");
    }
    EXPECT_TRUE(word);
}

TEST(DiffViewer, F7AndShiftF7MoveBetweenTheChanges) {
    FROST_REQUIRE_OFFSCREEN();
    DiffRig rig;
    // The caret starts on the first change.
    EXPECT_EQ(rig.viewer.currentBlock(), std::optional<std::size_t>(0));
    EXPECT_EQ(rig.rightLine(), 10);
    EXPECT_TRUE(rig.viewer.nextChange());
    EXPECT_EQ(rig.viewer.currentBlock(), std::optional<std::size_t>(1));
    EXPECT_EQ(rig.rightLine(), 50); // The deletion sits before right line 50.
    EXPECT_EQ(rig.viewer.leftPane()->textCursor().blockNumber(), 50);
    rig.press(Qt::Key_F7);
    EXPECT_EQ(rig.viewer.currentBlock(), std::optional<std::size_t>(2));
    EXPECT_EQ(rig.rightLine(), 120);
    EXPECT_FALSE(rig.viewer.nextChange()); // The last one.
    rig.press(Qt::Key_F7, Qt::ShiftModifier);
    EXPECT_EQ(rig.viewer.currentBlock(), std::optional<std::size_t>(1));
    EXPECT_TRUE(rig.viewer.previousChange());
    EXPECT_FALSE(rig.viewer.previousChange());
    EXPECT_EQ(rig.viewer.currentBlock(), std::optional<std::size_t>(0));
}

TEST(DiffViewer, ScrollsBothPanesTogether) {
    FROST_REQUIRE_OFFSCREEN();
    DiffRig rig;
    ASSERT_GT(rig.viewer.rightPane()->verticalScrollBar()->maximum(), 120);
    rig.viewer.scrollToLine(vcs::DiffSide::Right, 100);
    EXPECT_EQ(rig.viewer.topLine(vcs::DiffSide::Right), 100);
    // Right line 100 is left line 101: one line was deleted above it.
    EXPECT_EQ(rig.viewer.topLine(vcs::DiffSide::Left), 101);
    rig.viewer.scrollToLine(vcs::DiffSide::Left, 20);
    EXPECT_EQ(rig.viewer.topLine(vcs::DiffSide::Right), 20);
    rig.viewer.scrollToLine(vcs::DiffSide::Left, 130);
    EXPECT_EQ(rig.viewer.topLine(vcs::DiffSide::Right), 130); // -1 deleted, +1 added.
}

TEST(DiffViewer, AnEmptyDiffHasNoChanges) {
    FROST_REQUIRE_OFFSCREEN();
    DiffViewer viewer;
    auto same = vcs::SideBySideDiff::compute("same\n", "same\n");
    ASSERT_TRUE(same.has_value());
    viewer.setDiff(std::move(*same), QStringLiteral("HEAD"), QStringLiteral("Working copy"));
    EXPECT_EQ(viewer.currentBlock(), std::nullopt);
    EXPECT_FALSE(viewer.nextChange());
    EXPECT_FALSE(viewer.previousChange());
    EXPECT_EQ(viewer.highlightCount(vcs::DiffSide::Left), 0);
}

TEST(VcsColors, EveryGlacierThemeColoursEveryStatus) {
    FROST_REQUIRE_OFFSCREEN();
    for (const char* id : {"glacier-dark", "glacier-classic", "glacier-light", "glacier-high-contrast"}) {
        SCOPED_TRACE(id);
        AppearanceSettings settings;
        settings.theme = id;
        const auto installed = test::install(settings);
        ASSERT_NE(installed, nullptr);
        const FrostStyle* style = FrostStyle::current();
        ASSERT_NE(style, nullptr);
        EXPECT_EQ(vcsStatusColor(vcs::VcsFileStatus::Unmodified), std::nullopt);
        std::vector<QColor> seen;
        for (const auto status :
             {vcs::VcsFileStatus::Added, vcs::VcsFileStatus::Modified, vcs::VcsFileStatus::Deleted,
              vcs::VcsFileStatus::Untracked, vcs::VcsFileStatus::Conflicted, vcs::VcsFileStatus::Ignored}) {
            const std::optional<QColor> color = vcsStatusColor(status);
            ASSERT_TRUE(color.has_value()) << vcs::toString(status);
            EXPECT_TRUE(color->isValid());
            EXPECT_EQ(*color, style->color(*vcsColorKey(status)));
            EXPECT_EQ(std::ranges::find(seen, *color), seen.end()) << vcs::toString(status) << " repeats a colour";
            seen.push_back(*color);
        }
        // Renamed falls back to Modified; the editor's change stripes to the file colours.
        EXPECT_EQ(vcsStatusColor(vcs::VcsFileStatus::Renamed), vcsStatusColor(vcs::VcsFileStatus::Modified));
        EXPECT_EQ(style->color(ColorKey::EditorChangeAddedColor), style->color(ColorKey::VcsAdded));
        EXPECT_TRUE(style->color(ColorKey::DiffInsertedBackground).isValid());
        EXPECT_TRUE(style->color(ColorKey::DiffInnerChangeBackground).isValid());
        EXPECT_EQ(DiffColors::current().inserted, style->color(ColorKey::DiffInsertedBackground));
    }
}

TEST(VcsColors, TheFileColourProviderFollowsTheStatusMap) {
    vcs::VcsStatusMap map(std::string(kWorkTree), sampleStatus(), 1);
    const auto provider = vcsFileColorProvider(map);
    EXPECT_EQ(provider(at("src/main.cpp")), vcsStatusColor(vcs::VcsFileStatus::Modified));
    EXPECT_EQ(provider(at("notes.txt")), vcsStatusColor(vcs::VcsFileStatus::Untracked));
    EXPECT_EQ(provider(at("src/clean.cpp")), std::nullopt); // Unmodified.
#ifdef _WIN32
    EXPECT_EQ(provider("D:/elsewhere/a.cpp"), std::nullopt);
#else
    EXPECT_EQ(provider("/elsewhere/a.cpp"), std::nullopt);
#endif
}

TEST(GitShortcuts, TheDefaultsParseAndStayOffTheKeymapsIds) {
    const auto bindings = gitShortcutBindings();
    ASSERT_TRUE(bindings.has_value()) << bindings.error().describe();
    ASSERT_EQ(bindings->size(), 3U); // Show Diff, Next and Previous Change; the others are unbound.
    EXPECT_EQ((*bindings)[0].actionId, git_actions::kShowDiff);
    EXPECT_EQ((*bindings)[1].actionId, git_actions::kNextChange);
    EXPECT_EQ((*bindings)[2].actionId, git_actions::kPreviousChange);
    for (const input::Binding& binding : *bindings) {
        ASSERT_EQ(binding.sequences.size(), 1U);
    }
    EXPECT_EQ((*bindings)[0].sequences[0], input::parseSequence("Ctrl+D", input::SyntaxMode::Strict).value());
    EXPECT_EQ((*bindings)[2].sequences[0], input::parseSequence("Shift+F7", input::SyntaxMode::Strict).value());
    // The keymap already names vcs.commit, vcs.push, vcs.update and vcs.rollback: no module default.
    for (const GitShortcutDefault& entry : kGitShortcutDefaults) {
        EXPECT_NE(entry.actionId, git_actions::kCommit);
        EXPECT_NE(entry.actionId, git_actions::kPush);
        EXPECT_NE(entry.actionId, git_actions::kUpdateProject);
        EXPECT_NE(entry.actionId, git_actions::kRollback);
        EXPECT_TRUE(input::isValidActionId(entry.actionId)) << entry.actionId;
    }
}

} // namespace
} // namespace frost::ui
