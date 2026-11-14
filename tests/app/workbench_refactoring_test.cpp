// SPDX-License-Identifier: Apache-2.0
//
// REF-1 composition tests: one action spans files/views and Undo restores the complete operation.
#include "frost/document/refactoring_transaction.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "workbench_rig.hpp"
#include <gtest/gtest.h>

namespace frost::app::test {
namespace {
RigOptions optionsFor(const WorkDir& dir) {
    RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    return options;
}

TEST(WorkbenchRefactoring, OneUndoRestoresClosedFileAndOpenView) {
    WorkDir dir("refactoring-multifile");
    dir.write("workspace/a.cpp", "int oldName;\n");
    dir.write("workspace/b.cpp", "oldName = 1;\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/a.cpp")));
    document::RefactoringPlan plan;
    plan.label = "Rename";
    plan.buffers.push_back(
        {dir.join("workspace/a.cpp"), "int oldName;\n", "int newName;\n", document::BufferPersistence::Automatic});
    plan.disk.push_back({dir.join("workspace/b.cpp"), "oldName = 1;\n", "newName = 1;\n"});
    const auto applied = rig.workbench->applyRefactoring(plan);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, document::RefactoringStatus::Applied);
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "int newName;\n");
    EXPECT_EQ(dir.read("workspace/b.cpp"), "newName = 1;\n");
    EXPECT_TRUE(rig.workbench->canUndoRefactoring());
    const auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone);
    ASSERT_EQ(undone->status, document::RefactoringStatus::Undone);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "int oldName;\n");
    EXPECT_EQ(dir.read("workspace/a.cpp"), "int oldName;\n");
    EXPECT_EQ(dir.read("workspace/b.cpp"), "oldName = 1;\n");
    const auto redone = rig.workbench->redoRefactoring();
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->status, document::RefactoringStatus::Redone);
    EXPECT_EQ(rig.view()->editor().document().text().text(), "int newName;\n");
    EXPECT_EQ(dir.read("workspace/b.cpp"), "newName = 1;\n");
}

TEST(WorkbenchRefactoring, StaleParticipantRefusesAllChanges) {
    WorkDir dir("refactoring-stale");
    dir.write("workspace/a.cpp", "old\n");
    dir.write("workspace/b.cpp", "external\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    document::RefactoringPlan plan;
    plan.label = "Rename";
    plan.disk = {{dir.join("workspace/a.cpp"), "old\n", "new\n"}, {dir.join("workspace/b.cpp"), "old\n", "new\n"}};
    EXPECT_FALSE(rig.workbench->applyRefactoring(plan));
    EXPECT_EQ(dir.read("workspace/a.cpp"), "old\n");
    EXPECT_EQ(dir.read("workspace/b.cpp"), "external\n");
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
}

TEST(WorkbenchRefactoring, UntrustedPlanDoesNotChangeFiles) {
    WorkDir dir("refactoring-untrusted");
    dir.write("workspace/a.cpp", "old\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    document::RefactoringPlan plan;
    plan.label = "Rename";
    plan.disk.push_back({dir.join("workspace/a.cpp"), "old\n", "new\n"});
    const auto applied = rig.workbench->applyRefactoring(plan);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(dir.read("workspace/a.cpp"), "old\n");
}

TEST(WorkbenchRefactoring, FileRenameAndUndoRebindTheExistingView) {
    WorkDir dir("refactoring-rebind");
    dir.write("workspace/old.cpp", "int value;\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/old.cpp")));
    auto* original = rig.view();
    ASSERT_NE(original, nullptr);
    document::RefactoringPlan plan;
    plan.label = "Rename File";
    plan.renames.push_back({dir.join("workspace/old.cpp"), dir.join("workspace/new.cpp"), "int value;\n"});
    const auto applied = rig.workbench->applyRefactoring(plan);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, document::RefactoringStatus::Applied);
    EXPECT_EQ(rig.workbench->editors().find(dir.join("workspace/new.cpp")), original);
    EXPECT_EQ(original->document().path(), dir.join("workspace/new.cpp"));
    const auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone);
    ASSERT_EQ(undone->status, document::RefactoringStatus::Undone);
    EXPECT_EQ(rig.workbench->editors().find(dir.join("workspace/old.cpp")), original);
    EXPECT_EQ(original->document().path(), dir.join("workspace/old.cpp"));
    EXPECT_EQ(dir.read("workspace/old.cpp"), "int value;\n");
}

TEST(WorkbenchRefactoring, RevokedTrustDisablesUndoUntilRestored) {
    WorkDir dir("refactoring-revoked");
    dir.write("workspace/a.cpp", "old\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    document::RefactoringPlan plan;
    plan.label = "Rename";
    plan.disk.push_back({dir.join("workspace/a.cpp"), "old\n", "new\n"});
    const auto applied = rig.workbench->applyRefactoring(plan);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, document::RefactoringStatus::Applied);
    ASSERT_TRUE(rig.trust->untrust(dir.join("workspace")));
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    EXPECT_FALSE(rig.workbench->undoRefactoring());
    EXPECT_EQ(dir.read("workspace/a.cpp"), "new\n");
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    EXPECT_TRUE(rig.workbench->undoRefactoring());
    EXPECT_EQ(dir.read("workspace/a.cpp"), "old\n");
}

TEST(WorkbenchRefactoring, EditorUndoRoutesTheWholeGlobalBoundary) {
    WorkDir dir("refactoring-editor-undo");
    dir.write("workspace/a.cpp", "old\n");
    dir.write("workspace/b.cpp", "old\n");
    WorkbenchRig rig(dir, optionsFor(dir));
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/a.cpp")));
    document::RefactoringPlan plan;
    plan.label = "Rename";
    plan.buffers.push_back({dir.join("workspace/a.cpp"), "old\n", "new\n", document::BufferPersistence::Automatic});
    plan.disk.push_back({dir.join("workspace/b.cpp"), "old\n", "new\n"});
    const auto applied = rig.workbench->applyRefactoring(plan);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, document::RefactoringStatus::Applied);
    auto* editor = &rig.view()->editor();
    editor->setFocus();
    QApplication::processEvents();
    sendKey(editor, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->document().text().text(), "old\n");
    EXPECT_EQ(dir.read("workspace/b.cpp"), "old\n");
    EXPECT_TRUE(rig.workbench->canRedoRefactoring());
}
} // namespace
} // namespace frost::app::test
