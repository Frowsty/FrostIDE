// SPDX-License-Identifier: Apache-2.0
//
// REF-2: selected edits preserve server ranges; stale/missing sources cannot be accepted.
#include "frost/ui/lang/refactoring_preview.hpp"

#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

namespace {
namespace fl = frost::lang;
using frost::ui::lang::RefactoringPreview;

fl::WorkspaceEdit proposed() {
    return {.files = {{.path = "/project/a.cpp", .uri = "file:///project/a.cpp", .version = 12,
                       .edits = {{{{0, 0}, {0, 3}}, "newName"}, {{{0, 4}, {0, 7}}, "newName"}}},
                     {.path = "/project/b.cpp", .uri = "file:///project/b.cpp", .version = std::nullopt,
                       .edits = {{{{0, 0}, {0, 3}}, "newName"}}}}};
}

struct Files {
    std::unordered_map<std::string, std::string> text{{"/project/a.cpp", "old old\n"},
                                                    {"/project/b.cpp", "old\n"}};
    frost::Result<std::string> read(std::string_view path) {
        const auto it = text.find(std::string(path));
        if (it == text.end()) {
            return frost::fail(frost::ErrorCode::NotFound, "source removed");
        }
        return it->second;
    }
};

TEST(RefactoringPreview, ExcludingAnOccurrencePreservesOtherRangesAndVersions) {
    Files files;
    RefactoringPreview dialog(proposed(), fl::PositionEncoding::Utf16,
                               [&](std::string_view path) { return files.read(path); });
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("refactoringEdits"));
    ASSERT_NE(tree, nullptr);
    tree->topLevelItem(0)->child(0)->setCheckState(0, Qt::Unchecked);
    const auto selected = dialog.selectedEdit();
    ASSERT_EQ(selected.files.size(), 2U);
    ASSERT_EQ(selected.files[0].edits.size(), 1U);
    EXPECT_EQ(selected.files[0].version, 12);
    EXPECT_EQ(selected.files[0].edits[0], proposed().files[0].edits[1]);
    auto* after = dialog.findChild<QPlainTextEdit*>(QStringLiteral("refactoringAfter"));
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->toPlainText(), QStringLiteral("old newName\n"));
    EXPECT_TRUE(dialog.validateSelection());
    EXPECT_EQ(files.text.at("/project/a.cpp"), "old old\n");
}

TEST(RefactoringPreview, ExcludedFilesAreNeitherSubmittedNorRechecked) {
    Files files;
    RefactoringPreview dialog(proposed(), fl::PositionEncoding::Utf16,
                               [&](std::string_view path) { return files.read(path); });
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("refactoringEdits"));
    ASSERT_NE(tree, nullptr);
    tree->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
    files.text.erase("/project/b.cpp");
    ASSERT_EQ(dialog.selectedEdit().files.size(), 1U);
    EXPECT_TRUE(dialog.validateSelection());
}

TEST(RefactoringPreview, ChangedSelectedSourceRefusesAcceptWithoutWriting) {
    Files files;
    RefactoringPreview dialog(proposed(), fl::PositionEncoding::Utf16,
                               [&](std::string_view path) { return files.read(path); });
    bool accepted = false;
    QObject::connect(&dialog, &QDialog::accepted, [&] { accepted = true; });
    files.text["/project/a.cpp"] = "some unrelated change\n";
    const auto valid = dialog.validateSelection();
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().code(), frost::ErrorCode::Busy);
    dialog.accept();
    EXPECT_FALSE(accepted);
    EXPECT_EQ(files.text.at("/project/b.cpp"), "old\n");
}

TEST(RefactoringPreview, EmptySelectionDisablesApplyAndRejectsProgrammaticAccept) {
    Files files;
    RefactoringPreview dialog(proposed(), fl::PositionEncoding::Utf16,
                               [&](std::string_view path) { return files.read(path); });
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("refactoringEdits"));
    ASSERT_NE(tree, nullptr);
    tree->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
    tree->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
    const auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("applyRefactoring"));
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_FALSE(dialog.validateSelection());
}

TEST(RefactoringPreview, MissingSourceAtCaptureDisablesApply) {
    Files files;
    files.text.erase("/project/b.cpp");
    RefactoringPreview dialog(proposed(), fl::PositionEncoding::Utf16,
                               [&](std::string_view path) { return files.read(path); });
    EXPECT_FALSE(dialog.validateSelection());
    const auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("applyRefactoring"));
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
}
} // namespace
