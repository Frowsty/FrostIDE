// SPDX-License-Identifier: Apache-2.0
// Actual editor AST action -> source preview -> REF1 global history, authored for later execution.
#include "frost/lang/lua_service.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "workbench_rig.hpp"
#include <QDialog>
#include <QInputDialog>
#include <QTimer>
#include <gtest/gtest.h>
namespace frost::app::test {
TEST(WorkbenchLuaRefactoring, InlinePreviewDoesNotWriteBeforeAcceptanceAndGlobalUndoRestoresSource) {
    WorkDir dir("lua-inline-transaction");
    const std::string source = "local value=4\nprint(value)\n";
    dir.write("workspace/main.lua", source);
    RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/main.lua")));
    rig.show();
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    auto& editor = view->editor();
    editor.setFocus();
    const std::vector<text::Selection> carets{text::Selection::caret(ByteOffset{6})};
    editor.setState(text::EditorState::from(text::SelectionSet::fromSelections(carets, 0)));
    ASSERT_TRUE(waitFor([&] {
        auto* session = editor.syntaxSession();
        if (!session)
            return false;
        session->poll();
        return !session->tree().empty() && !session->tree().stale();
    }));
    bool preview = false;
    QTimer answer;
    answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->windowTitle() != QStringLiteral("Inline Lua local"))
            return;
        preview = true;
        EXPECT_EQ(dir.read("workspace/main.lua"), source);
        answer.stop();
        dialog->accept();
    });
    answer.start();
    EXPECT_EQ(rig.window->invoke("refactor.lua.inline"), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return editor.document().text().text().find("print((4))") != std::string::npos; }));
    EXPECT_TRUE(preview);
    auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone);
    EXPECT_EQ(editor.document().text().text(), source);
    auto redone = rig.workbench->redoRefactoring();
    ASSERT_TRUE(redone);
    EXPECT_NE(editor.document().text().text().find("print((4))"), std::string::npos);
}
TEST(WorkbenchLuaModuleReal, PinnedServiceResolvedRequireEditAndFileSidecarMoveShareOneHistoryEdge) {
    WorkDir dir("lua-module-real-transaction");
    dir.write("workspace/old.lua", "return {value=42}\n");
    dir.write("workspace/old.lua.fmeta", "sidecar bytes");
    dir.write("workspace/main.lua", "local m=require('old')\nprint(m.value)\n");
    dir.write("workspace/.luarc.json", "{\"runtime\":{\"path\":[\"?.lua\",\"?/init.lua\"],\"pathStrict\":true}}");
    RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    options.workbench.luaSearch = lang::defaultLuaSearch();
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openFolder(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/old.lua")));
    rig.show();
    rig.view()->editor().setFocus();
    ASSERT_TRUE(
        waitFor([&] { return rig.workbench->lua() && rig.workbench->lua()->state() == lang::LuaState::Running; }));
    bool preview = false;
    QTimer answer;
    answer.setInterval(1);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (auto* name = qobject_cast<QInputDialog*>(dialog)) {
            name->setTextValue("new.lua");
            name->accept();
            return;
        }
        if (dialog->windowTitle() != QStringLiteral("Rename Lua module and resolved workspace require users"))
            return;
        preview = true;
        EXPECT_TRUE(io::realFileSystem().exists(dir.join("workspace/old.lua")).value_or(false));
        EXPECT_FALSE(io::realFileSystem().exists(dir.join("workspace/new.lua")).value_or(true));
        EXPECT_NE(dir.read("workspace/main.lua").find("require('old')"), std::string::npos);
        answer.stop();
        dialog->accept();
    });
    answer.start();
    EXPECT_EQ(rig.window->invoke("refactor.lua.renameModule"), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return io::realFileSystem().exists(dir.join("workspace/new.lua")).value_or(false); }));
    EXPECT_TRUE(preview);
    EXPECT_EQ(dir.read("workspace/new.lua.fmeta"), "sidecar bytes");
    EXPECT_NE(dir.read("workspace/main.lua").find("require('new')"), std::string::npos);
    auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone);
    EXPECT_TRUE(io::realFileSystem().exists(dir.join("workspace/old.lua")).value_or(false));
    EXPECT_NE(dir.read("workspace/main.lua").find("require('old')"), std::string::npos);
    auto redone = rig.workbench->redoRefactoring();
    ASSERT_TRUE(redone);
    EXPECT_TRUE(io::realFileSystem().exists(dir.join("workspace/new.lua")).value_or(false));
    EXPECT_NE(dir.read("workspace/main.lua").find("require('new')"), std::string::npos);
}
} // namespace frost::app::test
