// SPDX-License-Identifier: Apache-2.0
//
// The workbench's LuaLS workspace wiring (LUA-3, LUA-4) with the pinned real LuaLS (required:
// these tests fail when it is missing): the Tools page's Lua runtime setting reaching the running
// server as didChangeConfiguration (no restart) with LuaLS's refresh requests handled, and the
// Settings dialog's Lua page writing the project's .luarc.json, after which LuaLS's diagnostics
// follow the new runtime. Expected diagnostics come from the oracle `lua-language-server --check`
// on a copy of the file (through IProcessLauncher), never from hard-coded lists.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_rc.hpp"
#include "frost/lang/lua_service.hpp"

#include "workbench_rig.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <utility>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using Found = std::set<std::pair<int, std::string>>; // (1-based line, LuaLS code).

constexpr std::string_view kSource = "local a = 1 << 2\n"
                                     "local b = 7 // 2\n"
                                     "local t <const> = 3\n"
                                     "print(a, b, t, frostProbeUndefined)\n";

frost::Result<std::string> pinnedLuaLs() {
    auto found = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!found)
        return frost::fail(frost::ErrorCode::NotFound,
                           "the pinned LuaLS " + std::string(frost::lang::pinnedLuaLanguageServerVersion()) +
                               " is required (tools/bootstrap): " + found.error().describe());
    return found;
}

// `lua-language-server --check` on kSource alone under `runtime` (lang's lua_workspace_e2e_test.cpp
// does the same); its log and meta go under `dir`/oracle-state.
Found oracle(const std::string& server, WorkDir& dir, std::string_view runtime) {
    std::string name(runtime);
    std::ranges::replace(name, ' ', '-');
    const std::string folder = dir.join("oracle-" + name);
    auto& fs = frost::io::realFileSystem();
    EXPECT_TRUE(fs.createDirectories(folder).has_value());
    EXPECT_TRUE(fs.writeFileAtomic(frost::io::joinPath(folder, "main.lua").value(), kSource).has_value());
    EXPECT_TRUE(fs.writeFileAtomic(frost::io::joinPath(folder, ".luarc.json").value(),
                                   frost::lang::Json::object()
                                       .set("runtime", frost::lang::Json::object().set("version", std::string(runtime)))
                                       .dump())
                    .has_value());
    const std::string out = dir.join("oracle-" + name + ".json");
    frost::io::ProcessSpec spec;
    spec.executable = server;
    spec.arguments =
        frost::lang::luaLanguageServerArguments(dir.join("oracle-state")).value_or(std::vector<std::string>{});
    spec.arguments.push_back("--check=" + folder);
    spec.arguments.emplace_back("--checklevel=Hint");
    spec.arguments.emplace_back("--check_format=json");
    spec.arguments.push_back("--check_out_path=" + out);
    spec.stdinMode = frost::io::Stdio::Null;
    spec.stdoutMode = frost::io::Stdio::Pipe;
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    auto process = frost::io::realProcessLauncher().launch(spec);
    EXPECT_TRUE(process.has_value());
    if (!process)
        return {};
    EXPECT_TRUE((*process)->communicate({}, 180s).has_value());
    Found found;
    auto text = fs.readFile(out);
    EXPECT_TRUE(text.has_value()) << out;
    if (auto report = text ? frost::lang::parseJson(*text) : frost::Result<frost::lang::Json>(frost::lang::Json());
        report && report->isObject()) {
        for (const auto& diagnostics : report->items())
            for (const auto& diagnostic : diagnostics.items())
                found.emplace(
                    static_cast<int>(diagnostic.find("range")->find("start")->integerAt("line").value_or(-1)) + 1,
                    std::string(diagnostic.stringAt("code")));
    }
    return found;
}

Found published(WorkbenchRig& rig, const std::string& file) {
    Found found;
    if (auto* lua = rig.workbench->lua())
        if (auto diagnostics = lua->diagnostics().forFile(file))
            for (const auto& item : diagnostics->diagnostics)
                found.emplace(item.line, item.code);
    return found;
}

// A trusted project with main.lua open in the running pinned LuaLS.
void openProject(WorkbenchRig& rig, WorkDir& dir, const std::string& server) {
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  server));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/main.lua")));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running; }, 30s));
}

TEST(WorkbenchLuaWorkspace, TheRuntimeSettingIsSentLiveWithoutARestart) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
    WorkDir dir("lua-runtime-live");
    const Found lua51 = oracle(*server, dir, "Lua 5.1");
    const Found lua54 = oracle(*server, dir, "Lua 5.4");
    ASSERT_NE(lua51, lua54) << "the oracle must tell the runtimes apart";
    dir.write("project/main.lua", kSource);
    frost::app::test::RigOptions options;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::luaRuntimeVersion,
                                  std::string("Lua 5.4")));
    rig.show();
    openProject(rig, dir, *server);
    const std::string file = dir.join("project/main.lua");
    EXPECT_TRUE(waitFor([&] { return published(rig, file) == lua54; }, 60s))
        << testing::PrintToString(published(rig, file));
    auto* service = rig.workbench->lua();
    ASSERT_NE(service, nullptr);
    const auto pid = service->serverPid();
    const auto sent = service->configurationChangesSent();

    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::luaRuntimeVersion,
                                  std::string("Lua 5.1")));
    EXPECT_TRUE(
        waitFor([&] { return rig.workbench->lua() && rig.workbench->lua()->configurationChangesSent() > sent; }, 20s));
    EXPECT_TRUE(waitFor([&] { return published(rig, file) == lua51; }, 60s))
        << testing::PrintToString(published(rig, file));
    EXPECT_EQ(rig.workbench->lua(), service) << "the same service, not a restart";
    EXPECT_EQ(service->serverPid(), pid);
    EXPECT_EQ(service->restarts(), 0U);
    // LuaLS asked for fresh semantic tokens after the runtime change, and the workbench handled it.
    EXPECT_TRUE(waitFor(
        [&] {
            const auto handled = rig.workbench->luaRefreshesHandled();
            return std::ranges::find(handled, std::string("workspace/semanticTokens/refresh")) != handled.end();
        },
        20s));
}

TEST(WorkbenchLuaWorkspace, TheLuaSettingsPageWritesLuarcAndLuaLsFollowsIt) {
    auto server = pinnedLuaLs();
    ASSERT_TRUE(server.has_value()) << server.error().describe();
    WorkDir dir("lua-settings-page");
    const Found lua51 = oracle(*server, dir, "Lua 5.1");
    const Found lua54 = oracle(*server, dir, "Lua 5.4");
    ASSERT_NE(lua51, lua54);
    dir.write("project/main.lua", kSource);
    dir.write("project/.luarc.json", R"({"$schema":"keep-me","runtime":{"version":"Lua 5.4"}})");
    frost::app::test::RigOptions options;
    options.workbench.clangd = false;
    WorkbenchRig rig(dir, options);
    rig.show();
    openProject(rig, dir, *server);
    const std::string file = dir.join("project/main.lua");
    EXPECT_TRUE(waitFor([&] { return published(rig, file) == lua54; }, 60s))
        << testing::PrintToString(published(rig, file));
    auto* service = rig.workbench->lua();
    ASSERT_NE(service, nullptr);
    const auto sent = service->configurationChangesSent();

    ASSERT_EQ(rig.window->invoke("file.settings"), frost::input::InvokeResult::Ran);
    QDialog* dialog = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("settingsDialog"))
                    dialog = qobject_cast<QDialog*>(widget);
            if (!dialog)
                dialog = rig.window->findChild<QDialog*>(QStringLiteral("settingsDialog"));
            return dialog != nullptr;
        },
        5s));
    auto* version = dialog->findChild<QComboBox*>(QStringLiteral("luarc.runtimeVersion"));
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    ASSERT_NE(version, nullptr);
    ASSERT_NE(buttons, nullptr);
    EXPECT_TRUE(version->isEnabled());
    EXPECT_EQ(version->currentData().toString(), QStringLiteral("Lua 5.4")) << "the page shows the file";
    const int lua51Index = version->findData(QStringLiteral("Lua 5.1"));
    ASSERT_GE(lua51Index, 0);
    version->setCurrentIndex(lua51Index);
    ASSERT_TRUE(buttons->button(QDialogButtonBox::Apply)->isEnabled());
    buttons->button(QDialogButtonBox::Apply)->click();

    auto rc = frost::lang::readLuaRc(frost::io::realFileSystem(), dir.join("project"));
    ASSERT_TRUE(rc.has_value()) << rc.error().describe();
    EXPECT_EQ(rc->settings.runtimeVersion, "Lua 5.1");
    EXPECT_NE(rc->text.find("keep-me"), std::string::npos) << "other keys survive: " << rc->text;
    EXPECT_TRUE(
        waitFor([&] { return rig.workbench->lua() && rig.workbench->lua()->configurationChangesSent() > sent; }, 20s));
    EXPECT_TRUE(waitFor([&] { return published(rig, file) == lua51; }, 60s))
        << testing::PrintToString(published(rig, file));
    EXPECT_EQ(rig.workbench->lua(), service);
    EXPECT_EQ(service->restarts(), 0U);
    buttons->button(QDialogButtonBox::Cancel)->click();
}

} // namespace
