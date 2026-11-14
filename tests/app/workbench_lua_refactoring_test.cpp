// SPDX-License-Identifier: Apache-2.0
// Actual editor AST action -> source preview -> REF1 global history, authored for later execution.
#include "workbench_rig.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/syntax/syntax_session.hpp"
#include <QDialog>
#include <QTimer>
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(WorkbenchLuaRefactoring, InlinePreviewDoesNotWriteBeforeAcceptanceAndGlobalUndoRestoresSource) {
    WorkDir dir("lua-inline-transaction"); const std::string source="local value=4\nprint(value)\n";
    dir.write("workspace/main.lua",source); RigOptions options;
    options.workbench.refactoringStateDirectory=dir.join("private-refactoring"); WorkbenchRig rig(dir,options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace"))); ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/main.lua"))); rig.show();
    auto* view=rig.view(); ASSERT_NE(view,nullptr); auto& editor=view->editor(); editor.setFocus();
    const std::vector<text::Selection> carets{text::Selection::caret(ByteOffset{6})};
    editor.setState(text::EditorState::from(text::SelectionSet::fromSelections(carets,0)));
    ASSERT_TRUE(waitFor([&]{auto* session=editor.syntaxSession();if(!session)return false;session->poll();return !session->tree().empty()&&!session->tree().stale();}));
    bool preview=false; QTimer answer;answer.setInterval(1);
    QObject::connect(&answer,&QTimer::timeout,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog||dialog->windowTitle()!=QStringLiteral("Inline Lua local"))return;
        preview=true; EXPECT_EQ(dir.read("workspace/main.lua"),source);answer.stop();dialog->accept();
    });answer.start();
    EXPECT_EQ(rig.window->invoke("refactor.lua.inline"),input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&]{return editor.document().text().text().find("print((4))")!=std::string::npos;}));
    EXPECT_TRUE(preview); auto undone=rig.workbench->undoRefactoring();ASSERT_TRUE(undone);
    EXPECT_EQ(editor.document().text().text(),source);
    auto redone=rig.workbench->redoRefactoring();ASSERT_TRUE(redone);
    EXPECT_NE(editor.document().text().text().find("print((4))"),std::string::npos);
}
}
