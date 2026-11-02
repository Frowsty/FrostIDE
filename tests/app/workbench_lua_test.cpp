// SPDX-License-Identifier: Apache-2.0
// Workbench Lua routing, trust and real LuaLS with engine-generated API definitions.
#include "frost/app/welcome_screen.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "workbench_rig.hpp"
#include <QByteArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <algorithm>
#include <chrono>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

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
        R"({"runtime":{"version":"Lua 5.4"},"workspace":{"library":[".frost/lua-api"],"checkThirdParty":false}})");
    dir.write("main.lua", "local x = AI.Wa\nAI.DoesNotExist()\nAI.Wake(nil)\nlocal score = 1\nprint(score)\n");
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
                    return std::ranges::any_of(
                        items, [](const auto& item) { return item.label.find("Wake") != std::string::npos; });
                },
                2s);
        },
        20s);
    EXPECT_TRUE(complete);
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
