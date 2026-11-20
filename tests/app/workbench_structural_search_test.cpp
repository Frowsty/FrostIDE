// SPDX-License-Identifier: Apache-2.0
// Normal structural action -> unchanged disk during preview -> one global REF1 Undo/Redo edge.
#include "frost/ui/editor/editor_view.hpp"
#include "workbench_rig.hpp"
#include <QDialog>
#include <QPlainTextEdit>
#include <QTimer>
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(WorkbenchStructuralSearch, NormalReplacePreviewAndUndoUseTheActualSyntaxMatches) {
    WorkDir dir("structural-replace-transaction");
    const std::string source = "void f(){int old=1; use(old); /* old */ const char* s=\"old\";}\n";
    dir.write("workspace/main.cpp", source);
    RigOptions options; options.workbench.clangd = false;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/main.cpp")));
    rig.show(); auto* view = rig.view(); ASSERT_NE(view, nullptr); view->editor().setFocus();
    bool patternSeen = false, previewSeen = false;
    QTimer answer; answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!dialog) return;
        if (dialog->objectName() == "structuralSearchPattern") {
            patternSeen = true;
            auto* pattern = dialog->findChild<QPlainTextEdit*>("structuralPattern");
            auto* replacement = dialog->findChild<QPlainTextEdit*>("structuralReplacement");
            ASSERT_NE(pattern, nullptr); ASSERT_NE(replacement, nullptr);
            pattern->setPlainText("((identifier) @match (#eq? @match \"old\"))");
            replacement->setPlainText("fresh"); dialog->accept();
        } else if (dialog->windowTitle() == "Structural Replace") {
            previewSeen = true; EXPECT_EQ(dir.read("workspace/main.cpp"), source);
            answer.stop(); dialog->accept();
        }
    }); answer.start();
    EXPECT_EQ(rig.window->invoke("refactor.structuralReplace"), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return view->editor().document().text().text().find("int fresh") != std::string::npos; }));
    EXPECT_TRUE(patternSeen); EXPECT_TRUE(previewSeen);
    EXPECT_EQ(view->editor().document().text().text(), "void f(){int fresh=1; use(fresh); /* old */ const char* s=\"old\";}\n");
    ASSERT_TRUE(rig.workbench->undoRefactoring()); EXPECT_EQ(view->editor().document().text().text(), source);
    ASSERT_TRUE(rig.workbench->redoRefactoring()); EXPECT_TRUE(view->editor().document().text().text().find("int fresh") != std::string::npos);
}
TEST(WorkbenchStructuralSearch, CancelledPatternDialogChangesNoDocumentOrDisk) {
    WorkDir dir("structural-cancel"); dir.write("workspace/main.cpp", "void f(){int old=1;}\n");
    RigOptions options; options.workbench.clangd = false;
    WorkbenchRig rig(dir, options); ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace"))); ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/main.cpp")));
    rig.show(); rig.view()->editor().setFocus();
    QTimer answer; answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog && dialog->objectName() == "structuralSearchPattern") { answer.stop(); dialog->reject(); }
    }); answer.start();
    EXPECT_EQ(rig.window->invoke("refactor.structuralReplace"), input::InvokeResult::Ran);
    EXPECT_EQ(dir.read("workspace/main.cpp"), "void f(){int old=1;}\n");
    EXPECT_EQ(rig.view()->editor().document().text().text(), "void f(){int old=1;}\n");
}
} // namespace frost::app::test
