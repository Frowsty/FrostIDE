// SPDX-License-Identifier: Apache-2.0
// Workbench Lua routing, trust and real LuaLS with engine-generated API definitions.
#include "frost/app/welcome_screen.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "workbench_rig.hpp"
#include <QByteArray>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>

#include <algorithm>
#include <chrono>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

TEST(WorkbenchLua, StructureRejectsReplyAfterSamePathReopen) {
    WorkDir dir("structure-reopen-lua");
    dir.write("main.lua", "-- \xf0\x9f\x98\x80 target\n");
    dir.write("structure-name", "old\n");
    dir.write("structure-hold", "hold\n");
    frost::app::test::RigOptions options;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  std::string(FROST_STRUCTURE_LSP)));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    rig.window->toolWindows().show("structure");
    auto* tree = rig.window->findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(waitFor([&] { return QFile::exists(QString::fromStdString(dir.join("structure-requested"))); }, 20s));
    ASSERT_TRUE(rig.window->tabs().close(0));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    dir.write("structure-name", "new\n");
    ASSERT_TRUE(QFile::remove(QString::fromStdString(dir.join("structure-hold"))));
    ASSERT_TRUE(waitFor([&] { return tree->topLevelItemCount() == 1; }, 20s));
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("new"));
    emit tree->itemActivated(tree->topLevelItem(0), 0);
    EXPECT_EQ(rig.workbench->currentView()->editor().editor().state().primary().head, frost::ByteOffset{8});
}

TEST(WorkbenchStructure, ClangdRestartChangesEncodingAndRefreshes) {
    WorkDir dir("structure-restart-lua");
    dir.write("main.cpp", "-- \xf0\x9f\x98\x80 target\n");
    dir.write("structure-name", "old\n");
    frost::app::test::RigOptions options;
    options.workbench.clangd = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsClangdPath,
                                  std::string(FROST_STRUCTURE_LSP)));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    rig.window->toolWindows().show("structure");
    auto* tree = rig.window->findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(waitFor([&] { return tree->topLevelItemCount() == 1; }, 20s));
    auto* service = rig.workbench->clangd();
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(service->encoding(), frost::lang::PositionEncoding::Utf16);
    dir.write("structure-name", "reloaded\n");
    dir.write("main.cpp", "-- \xf0\x9f\x98\x80 target -- reloaded\n");
    rig.workbench->checkExternalChanges();
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    ASSERT_TRUE(
        waitFor([&] { return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(0) == "reloaded"; }, 20s));
    dir.write("structure-name", "new\n");
    dir.write("structure-utf8", "utf8\n");
    dir.write("structure-hold", "hold\n");
    QFile::remove(QString::fromStdString(dir.join("structure-requested")));
    ASSERT_TRUE(service->killServerForTesting());
    ASSERT_TRUE(waitFor(
        [&] {
            return service->restarts() == 1 && service->state() == frost::lang::ClangdState::Running &&
                   service->encoding() == frost::lang::PositionEncoding::Utf8;
        },
        20s));
    EXPECT_EQ(rig.workbench->clangd(), service);
    EXPECT_EQ(service->encoding(), frost::lang::PositionEncoding::Utf8);
    ASSERT_TRUE(waitFor([&] { return QFile::exists(QString::fromStdString(dir.join("structure-requested"))); }, 20s));
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(dir.join("structure-hold"))));
    ASSERT_TRUE(
        waitFor([&] { return tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(0) == "new"; }, 20s));
    emit tree->itemActivated(tree->topLevelItem(0), 0);
    EXPECT_EQ(rig.workbench->currentView()->editor().editor().state().primary().head, frost::ByteOffset{8});
}

TEST(WorkbenchLua, FrostProjectIdentitySurvivesRecentAndSessionReopen) {
    WorkDir dir("frost-identity", "tiny_sln");
    dir.write("Game.frostproj", "{}");
    dir.write("main.txt", "session document\n");
    const std::string project = dir.join("Game.frostproj");
    {
        WorkbenchRig first(dir);
        ASSERT_TRUE(first.workbench->openPath(project));
        ASSERT_EQ(first.workbench->solution(), nullptr);
        ASSERT_TRUE(first.workbench->openPath(dir.join("main.txt")));
        ASSERT_FALSE(frost::app::recentProjects(*first.settings->snapshot()).empty());
        EXPECT_EQ(frost::app::recentProjects(*first.settings->snapshot()).front(), project);
    }
    {
        WorkbenchRig second(dir);
        // Another window may update recents while the last session still names the .frostproj.
        ASSERT_TRUE(frost::app::addRecentProject(*second.settings, dir.join("Tiny.sln")));
        ASSERT_TRUE(second.workbench->restoreLastProject());
        EXPECT_TRUE(second.workbench->hasProject());
        EXPECT_EQ(second.workbench->solution(), nullptr);
        ASSERT_EQ(second.window->tabs().count(), 1);
        EXPECT_EQ(second.window->tabs().editor(0)->path(), dir.join("main.txt"));
        ASSERT_TRUE(second.workbench->closeProject());
        auto* welcome = second.workbench->welcomeScreen();
        ASSERT_NE(welcome, nullptr);
        ASSERT_FALSE(welcome->recentProjects().empty());
        EXPECT_EQ(welcome->recentProjects().front(), project);
        welcome->activateRow(0);
        ASSERT_TRUE(waitFor([&] { return second.workbench->hasProject(); }));
        EXPECT_EQ(second.workbench->solution(), nullptr);
        ASSERT_TRUE(second.workbench->closeProject());
        // An explicit folder retains the normal one-solution discovery behavior.
        ASSERT_TRUE(second.workbench->openPath(dir.path()));
        EXPECT_NE(second.workbench->solution(), nullptr);
    }
}

TEST(WorkbenchLua, QueuedTrustAndSettingsCannotRestartAfterShutdown) {
    WorkDir dir("lua-shutdown");
    dir.write("main.lua", "print('hello')\n");
#ifdef _WIN32
    const std::string server = dir.join("lua-language-server.exe");
    dir.write("lua-language-server.exe", "fake server");
#else
    const std::string server = dir.join("lua-language-server");
    dir.write("lua-language-server", "fake server");
#endif
    frost::io::ScriptedProcessLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.workbench->shutdown();
    EXPECT_FALSE(waitFor([&] { return rig.workbench->lua() != nullptr; }, 1500ms));
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(WorkbenchLua, FormatsSelectionAndDocumentWithEditorConfigAndUndo) {
    auto server = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!server)
        GTEST_SKIP() << "LuaLS is not available";
    WorkDir dir("lua-formatting");
    const std::string original = "local function add(a,b) -- caf\xc3\xa9 \xf0\x9f\x98\x80\nreturn a+b\nend\n\n"
                                 "local function sub(a,b)\nreturn a-b\nend\n";
    const std::string selectionFormatted =
        "local function add(a, b) -- caf\xc3\xa9 \xf0\x9f\x98\x80\n  return a + b\nend\n\n"
        "local function sub(a,b)\nreturn a-b\nend\n";
    const std::string allFormatted = "local function add(a, b) -- caf\xc3\xa9 \xf0\x9f\x98\x80\n  return a + b\nend\n\n"
                                     "local function sub(a, b)\n  return a - b\nend\n";
    dir.write(".editorconfig", "root = true\n[*.lua]\nindent_style = space\nindent_size = 2\ntab_width = 8\n"
                               "[tabs.lua]\nindent_style = tab\nindent_size = tab\ntab_width = 8\n");
    dir.write(".luarc.json", R"({"format.defaultConfig":{"indent_style":"space","indent_size":"4"}})");
    dir.write("main.lua", original);
    dir.write("tabs.lua", original);
    frost::app::test::RigOptions options;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 20s));
    auto& editor = rig.workbench->currentView()->editor();
    EXPECT_EQ(editor.settings().tabSize, 8U);
    EXPECT_EQ(editor.settings().indentSize, 2U);
    EXPECT_TRUE(editor.settings().insertSpaces);
    const auto end = original.find("end\n") + 3;
    const std::vector<frost::text::Selection> selected{{frost::ByteOffset{0}, frost::ByteOffset{end}}};
    editor.setState(frost::text::EditorState::from(frost::text::SelectionSet::fromSelections(selected, 0)));
    rig.workbench->language()->reformat();
    ASSERT_TRUE(waitFor([&] { return editor.document().text().text() == selectionFormatted; }, 20s));
    EXPECT_TRUE(rig.workbench->currentView()->document().dirty());
    ASSERT_TRUE(editor.perform(frost::ui::EditorAction::Undo));
    EXPECT_EQ(editor.document().text().text(), original);
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{0}));
    rig.workbench->language()->reformat();
    ASSERT_TRUE(waitFor([&] { return editor.document().text().text() == allFormatted; }, 20s));
    ASSERT_TRUE(editor.perform(frost::ui::EditorAction::Undo));
    EXPECT_EQ(editor.document().text().text(), original);
    ASSERT_TRUE(editor.perform(frost::ui::EditorAction::Redo));
    EXPECT_EQ(editor.document().text().text(), allFormatted);
    EXPECT_EQ(dir.read("main.lua"), original);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("tabs.lua")));
    auto& tabsEditor = rig.workbench->currentView()->editor();
    EXPECT_EQ(tabsEditor.settings().tabSize, 8U);
    EXPECT_FALSE(tabsEditor.settings().insertSpaces);
    const std::string tabsFormatted =
        "local function add(a, b) -- caf\xc3\xa9 \xf0\x9f\x98\x80\n\treturn a + b\nend\n\n"
        "local function sub(a, b)\n\treturn a - b\nend\n";
    rig.workbench->language()->reformat();
    ASSERT_TRUE(waitFor([&] { return tabsEditor.document().text().text() == tabsFormatted; }, 20s));
    EXPECT_EQ(dir.read("tabs.lua"), original);
}

TEST(WorkbenchLua, GenericAnnotationsAndRequireNavigateAcrossFiles) {
    auto server = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!server)
        GTEST_SKIP() << "LuaLS is not available";
    WorkDir dir("lua-generic-module");
    const std::string main =
        "local M = require(\"module\")\nlocal s = \"\xf0\x9f\x98\x80\"; local x = M.identity(\"h\xc3\xa9\")\n";
    dir.write(".luarc.json", R"({"runtime.version":"Lua 5.4"})");
    dir.write("module.lua", "---@generic T\n---@param value T\n---@return T\n"
                            "local function identity(value) return value end\nreturn { identity = identity }\n");
    dir.write("main.lua", main);
    dir.write("main.cpp", "int main() { return 0; }\n");
    frost::app::test::RigOptions options;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 20s));
    auto& editor = rig.workbench->currentView()->editor();
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{main.find("identity") + 3}));
    ASSERT_TRUE(waitFor(
        [&] {
            rig.workbench->language()->showQuickDocumentation();
            return waitFor(
                [&] {
                    const auto& popup = rig.workbench->language()->quickDocPopup();
                    return popup.isVisible() && popup.plainText().contains(QStringLiteral("identity"));
                },
                2s);
        },
        20s));
    const QString help = rig.workbench->language()->quickDocPopup().plainText();
    EXPECT_TRUE(help.contains(QStringLiteral("identity"))) << help.toStdString();
    EXPECT_TRUE(help.contains(QStringLiteral("<T>"))) << help.toStdString();
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{main.find("module") + 2}));
    rig.workbench->language()->gotoDeclaration();
    ASSERT_TRUE(waitFor(
        [&] {
            auto* view = rig.workbench->currentView();
            return view && view->path() == dir.join("module.lua");
        },
        20s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    EXPECT_FALSE(rig.workbench->lua()->documentVersion(dir.join("main.cpp")).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    EXPECT_TRUE(rig.workbench->lua()->documentVersion(dir.join("main.lua")).has_value());
}

TEST(WorkbenchLua, TrustGateAndGeneratedApiReachEditor) {
    auto luals = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    QByteArray apiPath = qgetenv("FROST_TEST_LUA_API");
    if (!luals || apiPath.isEmpty())
        GTEST_SKIP() << "LuaLS or FROST_TEST_LUA_API not available";
    auto api = frost::io::realFileSystem().readFile(apiPath.toStdString());
    ASSERT_TRUE(api.has_value());
    WorkDir dir("lua-workbench");
    dir.write("Game.frostproj", "{}");
    dir.write("Unrelated.sln", "not a solution"); // A frostproj selects its own directory, not this file.
    dir.write(".frost/lua-api/frost.lua", *api);
    dir.write(
        ".luarc.json",
        R"({"runtime":{"version":"Lua 5.4"},"workspace":{"library":[".frost/lua-api"],"checkThirdParty":false},"completion":{"callSnippet":"Both"}})");
    dir.write("main.lua", "local s = \"\xf0\x9f\x98\x80\"; local x = AI.Wa\nAI.DoesNotExist()\nAI.Wake(nil)\nlocal "
                          "score = 1\nprint(score)\nlocal t = \"\xf0\x9f\x98\x80\"; AI.Wake");
    dir.write("main.cpp", "int main() { return 0; }\n");
    frost::app::test::RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *luals));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua"), {1, 16}));
    auto& initialEditor = rig.workbench->currentView()->editor();
    const auto initialText = initialEditor.document().text().text();
    ASSERT_NE(initialText.find("\xf0\x9f\x98\x80"), std::string::npos);
    ASSERT_NE(initialText.find("AI.Wa"), std::string::npos);
    const auto completionEnd = initialText.find("AI.Wa") + 5;
    initialEditor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{completionEnd}));
    EXPECT_EQ(rig.workbench->lua(), nullptr); // No process before trust.
    if (auto* clangd = rig.workbench->clangd())
        EXPECT_FALSE(clangd->documentVersion(dir.join("main.lua")).has_value());
    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr);
    auto* description = dialog->findChild<QLabel*>(QStringLiteral("trustDescription"));
    ASSERT_NE(description, nullptr);
    EXPECT_TRUE(description->text().contains(QStringLiteral("Lua language server")));
    EXPECT_TRUE(description->text().contains(QStringLiteral("scripts")));
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 20s));
    auto* lua = rig.workbench->lua();
    ASSERT_TRUE(waitFor(
        [&] {
            auto set = lua->diagnostics().forFile(dir.join("main.lua"));
            return set && std::ranges::any_of(set->diagnostics, [](const auto& d) {
                       return d.message.find("DoesNotExist") != std::string::npos;
                   });
        },
        20s));
    const auto diagnostic = lua->diagnostics().forFile(dir.join("main.lua"));
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_FALSE(std::ranges::any_of(diagnostic->diagnostics, [](const auto& item) {
        return item.code == "undefined-global" && item.message.find("AI") != std::string::npos;
    }));
    if (auto* clangd = rig.workbench->clangd())
        EXPECT_FALSE(clangd->documentVersion(dir.join("main.lua")).has_value());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->currentView()->editor().diagnostics().empty(); }, 5s));
    bool complete = waitFor(
        [&] {
            rig.workbench->language()->completeCode();
            return waitFor(
                [&] {
                    const auto items = rig.workbench->language()->completionPopup().model().items();
                    return std::ranges::any_of(items, [](const auto& item) {
                        return item.snippet && item.label.find("Wake") != std::string::npos;
                    });
                },
                2s);
        },
        20s);
    ASSERT_TRUE(complete);
    auto& liveEditor = rig.workbench->currentView()->editor();
    auto& completion = rig.workbench->language()->completionPopup();
    std::optional<std::size_t> snippetRow;
    for (std::size_t row = 0; row < completion.model().entries().size(); ++row) {
        const auto& item = completion.model().items()[completion.model().entries()[row].item];
        if (item.snippet && item.label.find("Wake") != std::string::npos) {
            snippetRow = row;
            break;
        }
    }
    ASSERT_TRUE(snippetRow.has_value());
    for (std::size_t step = 0;
         step < completion.model().entries().size() && completion.model().selected() != snippetRow; ++step)
        frost::app::test::sendKey(&liveEditor, Qt::Key_Down, Qt::NoModifier);
    ASSERT_EQ(completion.model().selected(), snippetRow);
    frost::app::test::sendKey(&liveEditor, Qt::Key_Return, Qt::NoModifier);
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->parameterInfoPopup().isVisible(); }, 15s));
    EXPECT_TRUE(rig.workbench->language()->parameterInfoPopup().html().contains(QStringLiteral("entity")));
    const auto placeholder = liveEditor.editor().state().primary().range();
    EXPECT_EQ(liveEditor.document().text().text(placeholder), "entity");
    for (const QChar character : QStringLiteral("guard"))
        frost::app::test::sendKey(&liveEditor, character.toUpper().unicode(), Qt::NoModifier, QString(character));
    EXPECT_NE(liveEditor.document().text().text().find("AI.Wake(guard)"), std::string::npos);
    frost::app::test::sendKey(&liveEditor, Qt::Key_Escape, Qt::NoModifier);
    EXPECT_FALSE(rig.workbench->language()->parameterInfoPopup().isVisible());
    const auto callOffset = liveEditor.document().text().text().rfind("AI.Wake") + 7;
    liveEditor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{callOffset}));
    frost::app::test::sendKey(&liveEditor, Qt::Key_ParenLeft, Qt::ShiftModifier, QStringLiteral("("));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->parameterInfoPopup().isVisible(); }, 15s))
        << liveEditor.document().text().text();
    EXPECT_TRUE(rig.workbench->language()->parameterInfoPopup().html().contains(QStringLiteral("entity")));
    rig.workbench->currentView()->editor().goToLine(3, 5);
    rig.workbench->language()->showQuickDocumentation();
    EXPECT_TRUE(waitFor([&] { return rig.workbench->language()->quickDocPopup().isVisible(); }, 10s));
    EXPECT_FALSE(rig.workbench->language()->quickDocPopup().plainText().isEmpty());
    rig.workbench->language()->gotoDeclaration();
    EXPECT_TRUE(waitFor(
        [&] {
            auto* current = rig.workbench->currentView();
            return current && current->path() == dir.join(".frost/lua-api/frost.lua");
        },
        10s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua"), {4, 8}));
    rig.workbench->language()->rename();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->renameInput().isVisible(); }, 10s));
    auto* rename = rig.workbench->language()->renameInput().field();
    rename->setText(QStringLiteral("points"));
    frost::app::test::sendKey(rename, Qt::Key_Return, Qt::NoModifier);
    EXPECT_TRUE(waitFor(
        [&] {
            const auto text = rig.workbench->currentView()->editor().editor().text().text();
            return text.find("local points = 1") != std::string::npos &&
                   text.find("print(points)") != std::string::npos;
        },
        10s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    EXPECT_FALSE(rig.workbench->language()->completionPopup().isVisible());
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    EXPECT_TRUE(waitFor([&] { return !rig.workbench->currentView()->editor().semanticTokens().empty(); }, 20s));
    EXPECT_FALSE(rig.workbench->clangd()->documentVersion(dir.join("main.lua")).has_value());
    ASSERT_TRUE(rig.trust->untrust(dir.path()));
    EXPECT_TRUE(waitFor([&] { return rig.workbench->lua() == nullptr; }, 5s));
}
} // namespace
