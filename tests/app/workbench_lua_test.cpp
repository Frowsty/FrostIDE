// SPDX-License-Identifier: Apache-2.0
// Workbench Lua routing, trust, the visible notice when LuaLS is missing, and the pinned real LuaLS
// (required: these tests fail when it is not installed) with engine-generated API definitions.
#include "frost/app/welcome_screen.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
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
#include <mutex>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The pinned LuaLS (ADR 0079) that tools/bootstrap installs; a missing one fails the test (LUA-1).
frost::Result<std::string> pinnedLuaLs() {
    auto found = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!found)
        return frost::fail(frost::ErrorCode::NotFound,
                           "the pinned LuaLS " + std::string(frost::lang::pinnedLuaLanguageServerVersion()) +
                               " is required (tools/bootstrap): " + found.error().describe());
    return found;
}

// The real launcher, recording the executable of every process it starts.
class RecordingLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        record(spec);
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        record(spec);
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] std::vector<std::string> executables() const {
        const std::scoped_lock lock(m_mutex);
        return m_executables;
    }

private:
    void record(const frost::io::ProcessSpec& spec) {
        const std::scoped_lock lock(m_mutex);
        m_executables.push_back(spec.executable);
    }
    mutable std::mutex m_mutex;
    std::vector<std::string> m_executables;
};

// The engine-generated API when FROST_TEST_LUA_API names it, else a hand-written stand-in of the
// same shape: the AI table and AI.Wake(entity).
frost::Result<std::string> engineApi() {
    const QByteArray apiPath = qgetenv("FROST_TEST_LUA_API");
    if (!apiPath.isEmpty())
        return frost::io::realFileSystem().readFile(apiPath.toStdString());
    return std::string("---@meta\n---@class AI\nAI = {}\n---Wakes the entity.\n---@param entity any\n"
                       "function AI.Wake(entity) end\n");
}

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

TEST(WorkbenchStructure, SplitViewsSendOneDocumentChangeAndRetainOpenUntilLastView) {
    WorkDir dir("structure-split-notifications");
    dir.write("main.cpp", "-- \xf0\x9f\x98\x80 target\n");
    dir.write("structure-name", "symbol\n");
    dir.write("other.cpp", "int other;\n");
    dir.write("structure-log", "enabled\n");
    frost::app::test::RigOptions options;
    options.workbench.clangd = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsClangdPath,
                                  std::string(FROST_STRUCTURE_LSP)));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    rig.window->toolWindows().show("structure");
    const QString barrier = QString::fromStdString(dir.join("structure-requested"));
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    auto readLog = [&] { return dir.read("structure-notifications"); };
    auto count = [&](std::string_view needle) {
        const std::string log = readLog();
        int total = 0;
        for (std::size_t at = 0; (at = log.find(needle, at)) != std::string::npos; at += needle.size())
            ++total;
        return total;
    };
    auto& tabs = rig.window->tabs();
    QFile::remove(barrier);
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    EXPECT_EQ(count("notification textDocument/didOpen"), 1);
    EXPECT_EQ(count("notification textDocument/didChange"), 0);
    QFile::remove(barrier);
    ASSERT_TRUE(rig.workbench->currentView()->editor().typeText("x"));
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    EXPECT_EQ(count("notification textDocument/didChange"), 1);
    QFile::remove(barrier);
    ASSERT_TRUE(tabs.closeGroup());
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    EXPECT_EQ(count("notification textDocument/didClose"), 0);
    auto* service = rig.workbench->clangd();
    ASSERT_NE(service, nullptr);
    QFile::remove(barrier);
    ASSERT_TRUE(service->killServerForTesting());
    ASSERT_TRUE(waitFor([&] { return service->restarts() == 1 && QFile::exists(barrier); }, 20s));
    EXPECT_EQ(count("notification textDocument/didOpen"), 2);
    EXPECT_EQ(count("notification textDocument/didChange"), 1);
    QFile::remove(barrier);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("other.cpp")));
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    QFile::remove(barrier);
    ASSERT_TRUE(tabs.close(tabs.indexOf(dir.join("main.cpp"))));
    tabs.activate(tabs.currentIndex());
    ASSERT_TRUE(waitFor([&] { return QFile::exists(barrier); }, 20s));
    EXPECT_EQ(count("notification textDocument/didClose"), 1);
}

// The product's default search (no LuaLS setting) finds the pinned LuaLS that tools/bootstrap
// installs and starts it for a trusted folder (LUA-1). The other rigs search for no LuaLS.
TEST(WorkbenchLua, DefaultSearchStartsThePinnedLuaLs) {
    auto pinned = pinnedLuaLs();
    ASSERT_TRUE(pinned.has_value()) << pinned.error().describe();
    WorkDir dir("lua-default-search");
    dir.write("main.lua", "local x = 1\nprint(x)\n");
    RecordingLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.luaSearch = frost::lang::defaultLuaSearch();
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->snapshot()->get(frost::settings::key::toolsLuaLanguageServerPath).empty());
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 30s))
        << "the pinned LuaLS did not start from the default search";
    const auto launched = launcher.executables();
    EXPECT_TRUE(std::ranges::any_of(launched,
                                    [&](const std::string& exe) {
                                        return frost::io::fileName(exe) == frost::io::fileName(*pinned) &&
                                               frost::io::parentPath(exe) == frost::io::parentPath(*pinned);
                                    }))
        << "expected a launch of " << *pinned;
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
        ASSERT_TRUE(second.settings->set(frost::settings::Scope::Application,
                                         frost::settings::key::generalReopenLastProject, true)); // Opt-in.
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

// The number of "Lua language server unavailable" notifications so far.
std::size_t luaUnavailableNotices(WorkbenchRig& rig) {
    return static_cast<std::size_t>(std::ranges::count_if(rig.window->notifications().log(), [](const auto& entry) {
        return entry.notification.title == QStringLiteral("Lua language server unavailable");
    }));
}

// Tool ablation (ADR 0078): with no LuaLS anywhere, a FrostEngine project shows a visible notice
// naming the reason and LUA-1, and no Lua process or Lua result appears.
TEST(WorkbenchLua, MissingLuaLsIsAVisibleNoticeForAFrostEngineProject) {
    WorkDir dir("lua-missing-engine");
    dir.write("Game.frostproj", "{}");
    dir.write("main.lua", "print(undefinedThing)\n");
    frost::io::ScriptedProcessLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.luaSearch = frost::lang::LuaSearch{};
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_EQ(luaUnavailableNotices(rig), 1U);
    const auto it = std::ranges::find_if(rig.window->notifications().log(), [](const auto& entry) {
        return entry.notification.title == QStringLiteral("Lua language server unavailable");
    });
    EXPECT_TRUE(it->notification.text.contains(QStringLiteral("LUA-1"))) << it->notification.text.toStdString();
    EXPECT_TRUE(it->notification.text.contains(QString::fromUtf8(frost::lang::pinnedLuaLanguageServerVersion().data())))
        << it->notification.text.toStdString();
    EXPECT_EQ(it->notification.kind, frost::ui::NotificationKind::Warning);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    EXPECT_EQ(luaUnavailableNotices(rig), 1U) << "the notice appears once per project";
    EXPECT_EQ(rig.workbench->lua(), nullptr);
    EXPECT_FALSE(std::ranges::any_of(launcher.launches(), [](const auto& record) {
        return record.spec.executable.find("lua") != std::string::npos;
    })) << "no Lua process without LuaLS";
    EXPECT_TRUE(rig.workbench->currentView()->editor().diagnostics().empty());
}

// In a project that is not a FrostEngine one, the notice waits for the first Lua file.
TEST(WorkbenchLua, MissingLuaLsIsReportedWhenTheFirstLuaFileOpens) {
    WorkDir dir("lua-missing-folder");
    dir.write("main.lua", "print('hello')\n");
    dir.write("other.lua", "print('other')\n");
    dir.write("main.cpp", "int main() { return 0; }\n");
    frost::io::ScriptedProcessLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.luaSearch = frost::lang::LuaSearch{};
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp")));
    EXPECT_EQ(luaUnavailableNotices(rig), 0U) << "no Lua in sight yet";
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.lua")));
    EXPECT_EQ(luaUnavailableNotices(rig), 1U);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("other.lua")));
    EXPECT_EQ(luaUnavailableNotices(rig), 1U);
    EXPECT_EQ(rig.workbench->lua(), nullptr);
    EXPECT_FALSE(std::ranges::any_of(launcher.launches(), [](const auto& record) {
        return record.spec.executable.find("lua") != std::string::npos;
    })) << "no Lua process without LuaLS";
}

TEST(WorkbenchLua, FormatsSelectionAndDocumentWithEditorConfigAndUndo) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
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
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
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
    auto luals = pinnedLuaLs();
    ASSERT_TRUE(luals.has_value()) << luals.error().describe();
    auto api = engineApi();
    ASSERT_TRUE(api.has_value()) << api.error().describe();
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

// LUA-5 & LUA-7 Workbench acceptance: semantic tokens, inlay hints, Find Usages, Structure,
// hover and signature help in the active Lua editor match LuaLS's own responses.
TEST(WorkbenchLua, CodeInsightSemanticTokensAndInlayHintsMatchLuaLs) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
    WorkDir dir("lua-insight-workbench");
    const std::string source = "---Adds two numbers.\n"
                               "---@param left number\n"
                               "---@param right number\n"
                               "---@return number\n"
                               "local function addValues(left, right)\n"
                               "    return left + right\n"
                               "end\n"
                               "local total = addValues(10, 20)\n"
                               "local again = addValues(total, 5)\n"
                               "return again\n";
    dir.write("main.lua", source);
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

    // Semantic tokens and inlay hints arrive on the active Lua editor, and Lua comments are not
    // misclassified as inactive preprocessor regions.
    ASSERT_TRUE(waitFor([&] { return !editor.semanticTokens().empty() && !editor.inlayHints().empty(); }, 20s));
    EXPECT_TRUE(editor.inactiveRanges().empty());

    // Structure tool window shows `addValues` from textDocument/documentSymbol.
    rig.window->toolWindows().show("structure");
    auto* tree = rig.window->findChild<QTreeWidget*>(QStringLiteral("structureTree"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(waitFor(
        [&] {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                if (tree->topLevelItem(i)->text(0).contains(QStringLiteral("addValues")))
                    return true;
            }
            return false;
        },
        20s));

    // Find Usages (`textDocument/references`) on `addValues` finds the declaration + 2 calls.
    const auto fnOffset = source.find("addValues");
    ASSERT_NE(fnOffset, std::string::npos);
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{fnOffset + 2}));
    rig.workbench->language()->findUsages();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->usagesPanel().usageCount() >= 3U; }, 20s));
    EXPECT_EQ(rig.workbench->language()->usagesPanel().usageCount(), 3U);

    // Parameter info (`textDocument/signatureHelp`) inside `addValues(10, 20)`.
    const auto callArgOffset = source.find("addValues(10") + 10;
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{callArgOffset}));
    rig.workbench->language()->showParameterInfo();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->parameterInfoPopup().isVisible(); }, 20s));
    EXPECT_TRUE(rig.workbench->language()->parameterInfoPopup().html().contains(QStringLiteral("left")));
}

// LUA-6 Workbench acceptance: inspectLuaCode runs `lua-language-server --check`, populates the
// Problems tool window with the exact oracle results, refuses untrusted workspaces, and reports
// missing LuaLS visibly without inventing diagnostics.
TEST(WorkbenchLua, InspectLuaCodePopulatesProblemsAndMatchesCheckOracle) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
    WorkDir dir("lua-inspect-workbench");
    dir.write("scripts/one.lua", "local a = 1\nprint(a + missingGlobalOne)\n");
    dir.write("scripts/two.lua", "print(missingGlobalTwo)\n");

    // 1. Untrusted workspace refuses to launch LuaLS --check and offers the trust dialog.
    {
        frost::io::ScriptedProcessLauncher scripted;
        frost::app::test::RigOptions untrustedOpts;
        untrustedOpts.launcher = &scripted;
        untrustedOpts.workbench.clangd = false;
        WorkbenchRig untrustedRig(dir, untrustedOpts);
        untrustedRig.show();
        ASSERT_TRUE(untrustedRig.workbench->openPath(dir.path()));
        const auto res = untrustedRig.workbench->inspectLuaCode();
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), frost::ErrorCode::Untrusted);
        EXPECT_TRUE(scripted.launches().empty());
        EXPECT_NE(untrustedRig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog")), nullptr);
    }

    // 2. Missing LuaLS reports NotFound and posts the visible unavailable notice.
    {
        frost::io::ScriptedProcessLauncher scripted;
        frost::app::test::RigOptions missingOpts;
        missingOpts.launcher = &scripted;
        missingOpts.workbench.clangd = false;
        missingOpts.workbench.luaSearch = frost::lang::LuaSearch{};
        WorkbenchRig missingRig(dir, missingOpts);
        ASSERT_TRUE(missingRig.trust->trust(dir.path()));
        ASSERT_TRUE(missingRig.workbench->openPath(dir.path()));
        const auto res = missingRig.workbench->inspectLuaCode();
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), frost::ErrorCode::NotFound);
        EXPECT_EQ(luaUnavailableNotices(missingRig), 1U);
        EXPECT_TRUE(missingRig.workbench->problems()->model()->problems().empty());
    }

    // 3. Real pinned LuaLS: inspectLuaCode matches direct `lua-language-server --check` and
    // populates ProblemsView for unopened and open Lua scripts alike.
    frost::app::test::RigOptions options;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    const std::string scriptsScope = dir.join("scripts");
    const auto inspected = rig.workbench->inspectLuaCode(scriptsScope);
    ASSERT_TRUE(inspected.has_value()) << inspected.error().describe();
    EXPECT_GE(inspected->size(), 2U);
    EXPECT_TRUE(rig.window->toolWindows().isVisible("problems"));

    frost::lang::LuaInspectOptions oracleOpts;
    oracleOpts.directory = scriptsScope;
    oracleOpts.stateDirectory = dir.join("oracle-state");
    oracleOpts.search.configuredPath = *server;
    oracleOpts.trusted = true;
    const auto oracle =
        frost::lang::inspectLuaDirectory(frost::io::realProcessLauncher(), frost::io::realFileSystem(), oracleOpts);
    ASSERT_TRUE(oracle.has_value()) << oracle.error().describe();
    ASSERT_EQ(inspected->size(), oracle->size());

    const auto& problems = rig.workbench->problems()->model()->problems();
    EXPECT_TRUE(std::ranges::any_of(problems, [](const frost::ui::Problem& p) {
        return p.diagnostic.diagnostic.code == "undefined-global" &&
               p.diagnostic.diagnostic.message.find("missingGlobalOne") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(problems, [](const frost::ui::Problem& p) {
        return p.diagnostic.diagnostic.code == "undefined-global" &&
               p.diagnostic.diagnostic.message.find("missingGlobalTwo") != std::string::npos;
    }));
}

// FEC-10b Workbench acceptance: EngineLuaHostProvider generates frost_schema.lua from
// `FrostEditor schema.export`, overrides `.luarc.json`'s `workspace.library`, completes component
// entries, navigates to the generated stub on Go to Definition, and opens the C++ `file:line`
// from `source_location` on `navigate.cppBinding` (`gotoCppBinding`).
TEST(WorkbenchLua, SchemaExportMetaStubsCompleteAndNavigateToStubAndCppBinding) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();

    class SchemaAndRealLuaLauncher final : public frost::io::IProcessLauncher {
    public:
        explicit SchemaAndRealLuaLauncher(std::string schemaJson) : m_schemaJson(std::move(schemaJson)) {}
        frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
            if (spec.executable.find("FrostEditor") != std::string::npos) {
                frost::io::ScriptedProcessLauncher scripted;
                scripted.expect(frost::io::ScriptedProcess{.exitCode = 0, .stdoutBytes = m_schemaJson});
                return scripted.launch(spec);
            }
            return frost::io::realProcessLauncher().launch(spec);
        }
        frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
        launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
            return frost::io::realProcessLauncher().launchAsync(spec, context);
        }

    private:
        std::string m_schemaJson;
    };

    const std::string schemaEnvelope = R"JSON({
        "command": "schema.export",
        "ok": true,
        "result": {
            "schema": "frostengine.schema-export",
            "schemaVersion": 1,
            "engine": { "version": "0.1.0" },
            "costTags": ["O(1)", "PhysicsQuery"],
            "bindingKinds": ["Wrapped", "Raw", "LuaLibrary"],
            "components": [
                {
                    "name": "Transform",
                    "source": { "file": "Frost/src/Frost/Scene/Components/TransformComponent.h", "line": 4 },
                    "fields": [
                        {
                            "name": "Position",
                            "type": "Vec3",
                            "source": { "file": "Frost/src/Frost/Scene/Components/TransformComponent.h", "line": 5 }
                        },
                        {
                            "name": "Scale",
                            "type": "Vec3",
                            "source": { "file": "Frost/src/Frost/Scene/Components/TransformComponent.h", "line": 6 }
                        }
                    ]
                }
            ],
            "lua": {
                "version": "Lua 5.4",
                "scriptsRoot": "Assets/Scripts",
                "modulePathPattern": "Assets/Scripts/?.lua",
                "entries": [
                    {
                        "kind": "Component",
                        "scope": "Component",
                        "owner": "",
                        "name": "Transform",
                        "fullName": "Transform",
                        "qualifiedName": "Components/Transform",
                        "signature": "Transform",
                        "description": "Transform component proxy",
                        "parameters": [],
                        "returns": { "type": "", "description": "" },
                        "module": "Scene",
                        "readOnly": false,
                        "testModeOnly": false,
                        "binding": "Wrapped",
                        "cost": ["O(1)"],
                        "source": { "file": "Frost/src/Frost/Scripting/Bindings/ComponentBindings.cpp", "line": 2 }
                    },
                    {
                        "kind": "Type",
                        "scope": "Global",
                        "owner": "",
                        "name": "Entity",
                        "fullName": "Entity",
                        "qualifiedName": "Entity",
                        "signature": "Entity",
                        "description": "Entity handle",
                        "parameters": [],
                        "returns": { "type": "", "description": "" },
                        "module": "Scene",
                        "readOnly": false,
                        "testModeOnly": false,
                        "binding": "Wrapped",
                        "cost": ["O(1)"],
                        "source": { "file": "Frost/src/Frost/Scripting/Bindings/EntityBindings.cpp", "line": 2 }
                    },
                    {
                        "kind": "Method",
                        "scope": "Global",
                        "owner": "Entity",
                        "name": "GetComponent",
                        "fullName": "Entity:GetComponent",
                        "qualifiedName": "Entity:GetComponent",
                        "signature": "GetComponent(name: ComponentName): Component?",
                        "description": "Look up a component",
                        "parameters": [
                            { "name": "name", "type": "ComponentName", "optional": false, "default": "", "description": "Component name" }
                        ],
                        "returns": { "type": "Component?", "description": "Component proxy" },
                        "module": "Scene",
                        "readOnly": false,
                        "testModeOnly": false,
                        "binding": "Wrapped",
                        "cost": ["O(1)"],
                        "source": { "file": "Frost/src/Frost/Scripting/Bindings/EntityBindings.cpp", "line": 3 }
                    },
                    {
                        "kind": "Namespace",
                        "scope": "Global",
                        "owner": "",
                        "name": "Physics",
                        "fullName": "Physics",
                        "qualifiedName": "Physics",
                        "signature": "Physics",
                        "description": "Physics namespace",
                        "parameters": [],
                        "returns": { "type": "", "description": "" },
                        "module": "Physics",
                        "readOnly": false,
                        "testModeOnly": false,
                        "binding": "Wrapped",
                        "cost": ["O(1)"],
                        "source": { "file": "Frost/src/Frost/Scripting/Bindings/PhysicsBindings.cpp", "line": 2 }
                    },
                    {
                        "kind": "Function",
                        "scope": "Global",
                        "owner": "Physics",
                        "name": "Raycast",
                        "fullName": "Physics.Raycast",
                        "qualifiedName": "Physics.Raycast",
                        "signature": "Raycast(origin: Vec3, dir: Vec3): boolean",
                        "description": "Cast a ray",
                        "parameters": [
                            { "name": "origin", "type": "Vec3", "optional": false, "default": "", "description": "Origin" },
                            { "name": "dir", "type": "Vec3", "optional": false, "default": "", "description": "Direction" }
                        ],
                        "returns": { "type": "boolean", "description": "Hit" },
                        "module": "Physics",
                        "readOnly": false,
                        "testModeOnly": false,
                        "binding": "Wrapped",
                        "cost": ["PhysicsQuery"],
                        "source": { "file": "Frost/src/Frost/Scripting/Bindings/PhysicsBindings.cpp", "line": 4 }
                    }
                ],
                "aliases": [
                    { "name": "ComponentName", "description": "", "values": [{ "value": "Transform", "description": "" }] }
                ]
            },
            "events": [
                {
                    "name": "OnUpdate",
                    "signature": "OnUpdate(dt: number)",
                    "description": "Called every frame",
                    "parameters": [
                        { "name": "dt", "type": "number", "optional": false, "default": "", "description": "Delta time" }
                    ],
                    "module": "Core",
                    "source": { "file": "Frost/src/Frost/Scripting/ScriptEngine.cpp", "line": 3 }
                }
            ],
            "commands": []
        }
    })JSON";

    WorkDir dir("lua-fec10b-workbench");
    dir.write("Game.frostproj", "{}");
    // Even if .luarc.json points workspace.library to an empty .frost/lua-api, FrostIDE's
    // host-library-override.json routes LuaLS to the generated frost_schema.lua stub.
    dir.write(".luarc.json", R"({"runtime":{"version":"Lua 5.4"},"workspace":{"library":[".frost/lua-api"]}})");
#ifdef _WIN32
    dir.write("build/windows-debug/bin/FrostEditor.com", "binary");
#else
    dir.write("build/linux-debug/bin/FrostEditor", "binary");
#endif
    dir.write("Frost/src/Frost/Scene/Components/TransformComponent.h",
              "// 1\n// 2\n// 3\nstruct TransformComponent {\n    Vec3 Position;\n    Vec3 Scale;\n};\n");
    dir.write("Frost/src/Frost/Scripting/Bindings/ComponentBindings.cpp", "// 1\nvoid BindTransform() {}\n");
    dir.write("Frost/src/Frost/Scripting/Bindings/EntityBindings.cpp",
              "// 1\nvoid BindEntity() {}\nvoid EntityGetComponent() {}\n");
    dir.write("Frost/src/Frost/Scripting/Bindings/PhysicsBindings.cpp",
              "// 1\nvoid BindPhysics() {}\n// 3\nbool PhysicsRaycast() { return true; }\n");
    dir.write("Frost/src/Frost/Scripting/ScriptEngine.cpp", "// 1\n// 2\nvoid DispatchOnUpdate() {}\n");

    const std::string scriptText = "---@class PlayerScript : FrostScript\n"
                                   "local PlayerScript = {}\n"
                                   "function PlayerScript:OnUpdate(dt)\n"
                                   "    local t = self.Entity:GetComponent(\"Transform\")\n"
                                   "    local pos = t.\n"
                                   "    local hit = Physics.Raycast(t.Position, t.Position)\n"
                                   "end\n";
    dir.write("Assets/Scripts/Player.lua", scriptText);

    SchemaAndRealLuaLauncher launcher(schemaEnvelope);
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *server));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Scripts/Player.lua")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 20s));

    // 1. Completion on `t.` returns the exact Transform fields (`Position`, `Scale`).
    auto& editor = rig.workbench->currentView()->editor();
    const auto dotOffset = scriptText.find("local pos = t.") + 14;
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{dotOffset}));
    ASSERT_TRUE(waitFor(
        [&] {
            rig.workbench->language()->completeCode();
            return waitFor(
                [&] {
                    const auto items = rig.workbench->language()->completionPopup().model().items();
                    return items.size() == 2U;
                },
                2s);
        },
        20s));
    std::vector<std::string> actualFields;
    for (const auto& item : rig.workbench->language()->completionPopup().model().items())
        actualFields.push_back(item.label);
    std::ranges::sort(actualFields);
    const std::vector<std::string> expectedFields{"Position", "Scale"};
    EXPECT_EQ(actualFields, expectedFields);

    // 2. Go to Definition on `Physics.Raycast` opens the generated `frost_schema.lua` stub.
    const auto raycastOffset = scriptText.find("Physics.Raycast") + 10;
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{raycastOffset}));
    rig.workbench->language()->gotoDeclaration();
    ASSERT_TRUE(waitFor(
        [&] {
            auto* view = rig.workbench->currentView();
            return view && view->path().find("frost_schema.lua") != std::string::npos;
        },
        20s));

    // 3. Go to C++ Binding (`navigate.cppBinding`) from the generated stub itself AND from Player.lua
    // opens `PhysicsBindings.cpp` at line 4 (`source_location`).
    const auto cppBoundFromStub = rig.workbench->gotoCppBinding();
    ASSERT_TRUE(cppBoundFromStub.has_value()) << cppBoundFromStub.error().describe();
    EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("Frost/src/Frost/Scripting/Bindings/PhysicsBindings.cpp"));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 4);

    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Scripts/Player.lua")));
    rig.workbench->currentView()->editor().setState(
        frost::text::EditorState::caretAt(frost::ByteOffset{raycastOffset}));
    const auto cppBound = rig.workbench->gotoCppBinding();
    ASSERT_TRUE(cppBound.has_value()) << cppBound.error().describe();
    EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("Frost/src/Frost/Scripting/Bindings/PhysicsBindings.cpp"));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 4);

    // 4. Go to C++ Binding on `t.Position` opens `TransformComponent.h` at line 5.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Scripts/Player.lua")));
    const auto positionOffset = scriptText.find("t.Position") + 4;
    rig.workbench->currentView()->editor().setState(
        frost::text::EditorState::caretAt(frost::ByteOffset{positionOffset}));
    ASSERT_TRUE(rig.workbench->gotoCppBinding().has_value());
    EXPECT_EQ(rig.workbench->currentView()->path(), dir.join("Frost/src/Frost/Scene/Components/TransformComponent.h"));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 5);
}
} // namespace
