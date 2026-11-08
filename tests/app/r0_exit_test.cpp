// SPDX-License-Identifier: Apache-2.0
//
// The R0 exit criteria (PLAN.md section 7, R0 "Exit criteria"; ADR 0078) as workbench acceptance
// tests over the real tools, every process counted by a recording launcher over the real launcher:
//   - tool ablation: with clangd, cmake/ctest and LuaLS pointed nowhere, a C++ file, a CMake folder
//     and a .lua file show "unavailable" notices and no result (no diagnostics, semantic tokens,
//     structure, completion or test outcome), and none of those tools is started;
//   - navigation: Go to Definition into a library header outside the workspace (the real clangd)
//     opens that header and writes nothing into the workspace or the library;
//   - untrusted folders: an untrusted CMake folder and an untrusted .frostproj folder start no
//     build, no CMake configure and no FrostEngine command until trust is granted in the trust
//     dialog, and nothing is trusted silently;
//   - R0 follow-up 13: FrostEngine engine-helper launches (scripted, as in workbench_engine_test.cpp)
//     while the real pinned LuaLS runs for the same .frostproj folder: both coexist, every launch is
//     accounted for and the LuaLS stays Running.
// The profile data (settings, trust store, sessions) of every rig lives in a work directory of its
// own, apart from the project, so a "nothing was written" check sees only what the product wrote.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/rpc/json.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QCoreApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The executable's base name, lower-case, without ".exe": the tool a launch started.
std::string toolName(std::string_view executable) {
    std::string name(frost::io::fileName(executable));
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.ends_with(".exe"))
        name.resize(name.size() - 4);
    return name;
}

/*
 * The spawn log of a test: every launch's executable, in order. Launches of `scriptedExecutable`
 * (the FrostEngine request helper, which is this test binary unless the options name another) go to
 * a ScriptedProcessLauncher; every other launch goes to the real launcher. Thread-safe.
 */
class RecordingLauncher final : public frost::io::IProcessLauncher {
public:
    RecordingLauncher() = default;
    RecordingLauncher(std::string scriptedExecutable, frost::io::ScriptedProcessLauncher& scripted)
        : m_scriptedExecutable(std::move(scriptedExecutable)), m_scripted(&scripted) {}

    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        record(spec);
        if (scripted(spec))
            return m_scripted->launch(spec);
        if (auto refused = refuseThisBinary(spec); !refused)
            return std::unexpected(std::move(refused.error()));
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        record(spec);
        if (scripted(spec))
            return m_scripted->launchAsync(spec, context);
        if (auto refused = refuseThisBinary(spec); !refused)
            return std::unexpected(std::move(refused.error()));
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] std::vector<std::string> executables() const {
        const std::scoped_lock lock(m_mutex);
        return m_executables;
    }
    // Launches whose tool name (toolName) is one of `names`.
    [[nodiscard]] std::size_t count(std::initializer_list<std::string_view> names) const {
        const std::scoped_lock lock(m_mutex);
        return static_cast<std::size_t>(std::ranges::count_if(m_executables, [&](const std::string& exe) {
            return std::ranges::find(names, toolName(exe)) != names.end();
        }));
    }
    // Every launch, one per line, for failure messages.
    [[nodiscard]] std::string describe() const {
        std::string text;
        for (const std::string& exe : executables())
            text += "\n  " + exe;
        return text.empty() ? std::string(" (none)") : text;
    }

private:
    // This test binary is the default engine helper; it must never start for real (gtest would run
    // every test of the binary again inside the "helper").
    static frost::Result<void> refuseThisBinary(const frost::io::ProcessSpec& spec) {
        const std::string self = QCoreApplication::applicationFilePath().toStdString();
        if (frost::io::normalizePath(spec.executable).value_or(spec.executable) ==
            frost::io::normalizePath(self).value_or(self))
            return frost::fail(frost::ErrorCode::InvalidArgument, "the test refuses to start its own binary");
        return {};
    }
    [[nodiscard]] bool scripted(const frost::io::ProcessSpec& spec) const {
        return m_scripted != nullptr && spec.executable == m_scriptedExecutable;
    }
    void record(const frost::io::ProcessSpec& spec) {
        const std::scoped_lock lock(m_mutex);
        m_executables.push_back(spec.executable);
    }
    std::string m_scriptedExecutable;
    frost::io::ScriptedProcessLauncher* m_scripted = nullptr;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_executables;
};

// The tools whose launch would be a build, a configure or a tool result.
constexpr std::initializer_list<std::string_view> kBuildTools = {"cmake",    "ninja", "msbuild", "ctest", "cl",
                                                                 "clang-cl", "clang", "gcc",     "cc",    "make"};

// Every file under `root` (relative path -> size), recursively, except the subtrees named in
// `skip` (relative directory paths).
std::map<std::string, std::uint64_t> fileList(const std::string& root, const std::set<std::string>& skip = {}) {
    std::map<std::string, std::uint64_t> files;
    std::vector<std::string> pending{""};
    while (!pending.empty()) {
        const std::string relative = pending.back();
        pending.pop_back();
        const std::string directory = relative.empty() ? root : frost::io::joinPath(root, relative).value_or("");
        auto entries = frost::io::realFileSystem().listDirectory(directory);
        if (!entries)
            continue;
        for (const frost::io::DirEntry& entry : *entries) {
            const std::string child = relative.empty() ? entry.name : relative + "/" + entry.name;
            if (entry.type == frost::io::FileType::Directory) {
                if (!skip.contains(child))
                    pending.push_back(child);
                continue;
            }
            const auto info = frost::io::realFileSystem().status(frost::io::joinPath(root, child).value_or(""));
            files[child] = info ? info->size : 0;
        }
    }
    return files;
}

std::string describe(const std::map<std::string, std::uint64_t>& files) {
    std::string text;
    for (const auto& [path, size] : files)
        text += "\n  " + path + " (" + std::to_string(size) + " bytes)";
    return text;
}

// The notifications whose title is `title`.
std::vector<frost::ui::Notification> notices(WorkbenchRig& rig, const QString& title) {
    std::vector<frost::ui::Notification> found;
    for (const auto& entry : rig.window->notifications().log())
        if (entry.notification.title == title)
            found.push_back(entry.notification);
    return found;
}

std::string allNotices(WorkbenchRig& rig) {
    std::string text;
    for (const auto& entry : rig.window->notifications().log())
        text += "\n  " + entry.notification.title.toStdString() + ": " + entry.notification.text.toStdString();
    return text.empty() ? std::string(" (none)") : text;
}

// Lets queued events and worker results arrive for `duration` (nothing is waited for).
void settle(std::chrono::milliseconds duration) {
    static_cast<void>(waitFor([] { return false; }, duration));
}

QPushButton* button(QWidget* panel, const QString& text) {
    for (auto* candidate : panel->findChildren<QPushButton*>())
        if (candidate->text() == text)
            return candidate;
    return nullptr;
}
bool hasStatus(QWidget* panel, const QString& text) {
    for (auto* label : panel->findChildren<QLabel*>())
        if (label->text().contains(text))
            return true;
    return false;
}

// An absolute path in a directory that does not exist: a tool setting that points nowhere.
std::string nowhere(const WorkDir& profile, std::string_view tool) {
    return profile.join(std::string("no tools here/") + std::string(tool) +
                        (frost::io::kNativePathStyle == frost::io::PathStyle::Windows ? ".exe" : ""));
}

/*
 * Tool ablation (ADR 0078 truth rule; R0 exit, FND-3 acceptance). clangd, cmake (and with it ctest
 * and ninja) and LuaLS are configured to absolute paths that do not exist (findClangd and
 * findCMakeTools never fall back from a configured path) and LuaLS's search has no directory and no
 * PATH, in a trusted folder that has a CMakeLists.txt, C++ sources with a deliberate error, a
 * gtest-style test and a Lua script with an undefined global. The IDE must say each tool is
 * unavailable and show no result of any of them.
 */
TEST(R0ToolAblation, WithoutClangdCMakeAndLuaLsTheIdeSaysUnavailableAndShowsNoResults) {
    WorkDir profile("ablation-profile");
    WorkDir dir("ablation-project");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Ablation CXX)\n"
                                "add_executable(app main.cpp)\nenable_testing()\nadd_test(NAME app COMMAND app)\n");
    dir.write("util.hpp", "#pragma once\nstruct Widget {\n    int size = 0;\n};\ninline int helper() { return 0; }\n");
    dir.write("main.cpp",
              "#include \"util.hpp\"\nint main() {\n    Widget w;\n    return helper() + w.si + missing;\n}\n");
    dir.write("widget_test.cpp", "#include \"util.hpp\"\nTEST(WidgetSuite, StartsEmpty) {\n}\n");
    dir.write("script.lua", "local value = undefinedGlobal\nreturn value\n");

    RecordingLauncher launcher;
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.workbench.luaSearch = frost::lang::LuaSearch{};
    options.launcher = &launcher;
    WorkbenchRig rig(profile, options);
    using frost::settings::Scope;
    namespace key = frost::settings::key;
    ASSERT_TRUE(rig.settings->set(Scope::Application, key::toolsClangdPath, nowhere(profile, "clangd")));
    ASSERT_TRUE(rig.settings->set(Scope::Application, key::toolsCmakePath, nowhere(profile, "cmake")));
    ASSERT_TRUE(rig.settings->set(Scope::Application, key::toolsNinjaPath, nowhere(profile, "ninja")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));

    // CMake: the configure-on-open of the trusted folder fails before it starts, and says so.
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr) << "the folder has a CMakeLists.txt";
    ASSERT_TRUE(waitFor([&] { return service->lastConfigure().has_value(); }, 60s)) << "no configure outcome";
    const auto configure = *service->lastConfigure();
    ASSERT_TRUE(configure.error.has_value()) << "a configure without cmake cannot succeed";
    EXPECT_NE(configure.error->describe().find("cmake"), std::string::npos) << configure.error->describe();
    auto* banner = rig.window->findChild<QLabel*>(QStringLiteral("cmakeErrorBanner"));
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(waitFor([&] { return banner->text().contains(QStringLiteral("cmake"), Qt::CaseInsensitive); }, 10s))
        << "the CMake tool window shows why: '" << banner->text().toStdString() << "'";
    EXPECT_FALSE(service->model()) << "no CMake model without cmake";
    EXPECT_TRUE(service->diagnostics().empty());

    // C++: no clangd process, a visible notice naming the missing tool, no result in the editor.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.cpp"), {4, 23}));
    EXPECT_EQ(rig.workbench->clangd(), nullptr);
    const QString clangdTitle = QStringLiteral("C++ language server unavailable");
    ASSERT_TRUE(waitFor([&] { return !notices(rig, clangdTitle).empty(); }, 10s))
        << "notifications:" << allNotices(rig);
    settle(1500ms); // The 1 s trust watch and the configure's events: the notice stays one per project.
    const auto clangdNotices = notices(rig, clangdTitle);
    ASSERT_EQ(clangdNotices.size(), 1U) << "notifications:" << allNotices(rig);
    EXPECT_TRUE(clangdNotices.front().text.contains(QStringLiteral("clangd")))
        << clangdNotices.front().text.toStdString();
    EXPECT_EQ(clangdNotices.front().kind, frost::ui::NotificationKind::Warning);

    // Lua: the first .lua file reports the missing LuaLS (LUA-1) once; no Lua process.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("script.lua")));
    EXPECT_EQ(rig.workbench->lua(), nullptr);
    EXPECT_EQ(notices(rig, QStringLiteral("Lua language server unavailable")).size(), 1U)
        << "notifications:" << allNotices(rig);

    // Ask for every result: completion, structure, the Unit Tests window. None may appear.
    rig.window->toolWindows().show("structure");
    for (const auto& [file, line, column] : {std::tuple{"main.cpp", 4, 23}, std::tuple{"script.lua", 1, 20}}) {
        ASSERT_TRUE(rig.workbench->openPath(dir.join(file), {line, column}));
        rig.workbench->language()->completeCode();
        settle(2s); // Long enough for a server that existed to answer; none does.
        auto& view = *rig.workbench->currentView();
        EXPECT_TRUE(view.editor().diagnostics().empty()) << file;
        EXPECT_TRUE(view.editor().semanticTokens().empty()) << file;
        EXPECT_FALSE(rig.workbench->language()->completionPopup().isVisible()) << file;
        EXPECT_TRUE(rig.workbench->language()->completionPopup().model().items().empty()) << file;
        auto* structure = rig.window->findChild<QTreeWidget*>(QStringLiteral("structureTree"));
        ASSERT_NE(structure, nullptr);
        EXPECT_EQ(structure->topLevelItemCount(), 0) << file << ": a Structure without a language server";
    }
    rig.window->toolWindows().show(std::string(frost::app::kUnitTestsToolWindow));
    auto* tests = rig.window->findChild<QTreeWidget*>(QStringLiteral("unitTestsTreeWidget"));
    ASSERT_NE(tests, nullptr);
    // Tests come from ctest (RUN-7), which a folder that never configured does not have: no test,
    // no status, and the window says why.
    EXPECT_EQ(tests->topLevelItemCount(), 0) << "a test list without ctest";
    auto* testsSummary = rig.window->findChild<QLabel*>(QStringLiteral("unitTestsSummaryLabel"));
    ASSERT_NE(testsSummary, nullptr);
    EXPECT_TRUE(testsSummary->text().contains(QStringLiteral("not configured"))) << testsSummary->text().toStdString();
    for (const char* name : {"unitTestsRunAllButton", "unitTestsRunSelectedButton", "unitTestsRunFailedButton"}) {
        auto* run = rig.window->findChild<QPushButton*>(QString::fromLatin1(name));
        ASSERT_NE(run, nullptr) << name;
        EXPECT_FALSE(run->isEnabled()) << name;
    }

    // Nothing that produces results was started: no clangd, no cmake, ninja or ctest, no LuaLS.
    EXPECT_EQ(launcher.count({"clangd", "lua-language-server"}), 0U) << "launches:" << launcher.describe();
    EXPECT_EQ(launcher.count(kBuildTools), 0U) << "launches:" << launcher.describe();
}

/*
 * Go to Definition into a library (R0 exit; FND-3 acceptance: the decompiler path that wrote
 * synthetic source into the workspace is gone). The library is a header in a directory outside the
 * workspace, included through -isystem. After the real clangd's answer opens it, the workspace and
 * the library hold exactly the files they held before; "Decompile" on the same call writes nothing
 * either. The one exception is clangd's own background index under <database dir>/.cache/clangd,
 * which the real tool writes for the translation units of the database (not the product, and not a
 * source file); it is listed separately and must stay inside that directory.
 */
TEST(R0Navigation, GoToDefinitionIntoALibraryHeaderLeavesTheWorkspaceFileListUnchanged) {
    frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir profile("nav-profile");
    WorkDir library("nav-library");
    WorkDir dir("nav-workspace");
    const std::string header = "#pragma once\nnamespace mathlib {\n/// Twice the value.\n"
                               "inline int twice(int value) { return value * 2; }\n} // namespace mathlib\n";
    library.write("include/mathlib/mathlib.hpp", header);
    dir.write("main.cpp", "#include <mathlib/mathlib.hpp>\nint main() { return mathlib::twice(21); }\n");
    const std::string main = dir.join("main.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = dir.path(),
        .file = main,
        .arguments = {"clang++", "-std=c++20", "-isystem", library.join("include"), "-c", main}}};
    ASSERT_TRUE(
        frost::lang::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands).has_value());
    // The oracle of the target: the header's own text says where twice is defined (1-based line).
    const std::string libraryHeader = library.join("include/mathlib/mathlib.hpp");
    const int definitionLine =
        1 + static_cast<int>(std::ranges::count(header.substr(0, header.find("inline int twice")), '\n'));

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    WorkbenchRig rig(profile, std::move(options));
    rig.show();
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    const std::set<std::string> clangdIndex = {"build/.cache"};
    const auto workspaceBefore = fileList(dir.path(), clangdIndex);
    const auto libraryBefore = fileList(library.path());
    ASSERT_EQ(workspaceBefore.size(), 2U) << describe(workspaceBefore); // main.cpp, build/compile_commands.json.

    // The caret on "twice" in the call (line 2).
    const auto atTheCall = [&] {
        const std::string line = "int main() { return mathlib::twice(21); }";
        EXPECT_TRUE(rig.workbench->openPath(main, {2, static_cast<int>(line.find("twice")) + 2}).has_value());
        return rig.workbench->currentView();
    };
    const auto onTheHeader = [&] {
        const auto* current = rig.workbench->currentView();
        return current != nullptr && current->path() == libraryHeader;
    };
    // clangd answers once it has parsed main.cpp; until then a request may come back empty.
    bool arrived = false;
    for (const auto deadline = std::chrono::steady_clock::now() + 120s;
         !arrived && std::chrono::steady_clock::now() < deadline;) {
        ASSERT_NE(atTheCall(), nullptr);
        rig.workbench->language()->gotoDeclaration();
        arrived = waitFor(onTheHeader, 3s);
    }
    ASSERT_TRUE(arrived) << "Go to Definition did not open " << libraryHeader;
    EXPECT_EQ(rig.workbench->currentView()->status().line, definitionLine);
    EXPECT_EQ(rig.workbench->currentView()->editor().document().text().text(), header)
        << "the header is shown as it is";

    // Decompile on the same call: C++ has nothing to decompile, and nothing may be synthesized.
    ASSERT_NE(atTheCall(), nullptr);
    rig.workbench->language()->decompileExternalSymbol();
    settle(3s);

    const auto workspaceAfter = fileList(dir.path(), clangdIndex);
    EXPECT_EQ(workspaceAfter, workspaceBefore)
        << "before:" << describe(workspaceBefore) << "\nafter:" << describe(workspaceAfter);
    EXPECT_EQ(fileList(library.path()), libraryBefore) << "nothing is written next to the library header";
    // Whatever else appeared is clangd's index, under build/.cache/clangd only.
    for (const auto& [path, size] : fileList(dir.path()))
        if (!workspaceBefore.contains(path))
            EXPECT_TRUE(path.starts_with("build/.cache/clangd/")) << path;
}

// A C project with one Debug Ninja profile; the compiler is the build's own.
void writeCMakeProject(WorkDir& dir) {
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                "set(CMAKE_C_COMPILER \"" +
                                    compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\n"
                                    "project(Untrusted C)\nadd_executable(hello main.c)\n");
    dir.write("main.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-debug"}]})");
}

/*
 * An untrusted CMake folder (R0 exit; FND-3 acceptance "spawns nothing before trust"). Opening it,
 * asking for a build and reloading the CMake project start no process at all and trust nothing;
 * the trust dialog is the only way in. Granting trust there starts the real configure, which the
 * same spawn log then records (so the log is known to see cmake).
 */
TEST(R0Trust, AnUntrustedCMakeFolderRunsNoBuildOrConfigureUntilTrustIsGranted) {
    WorkDir profile("untrusted-cmake-profile");
    WorkDir dir("untrusted-cmake");
    writeCMakeProject(dir);
    RecordingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(profile, options);
    rig.show();
    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.c")));
    settle(3s); // A configure-on-open or a trust watch tick would have started by now.

    // Explicit requests that need trust: a build and Reload CMake Project.
    ASSERT_TRUE(rig.workbench->build().build(frost::build::BuildAction::Build).has_value());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().state() == frost::build::BuildState::NeedsTrust; }, 30s));
    auto* reload = rig.window->findChild<QToolButton*>(QStringLiteral("cmakeReload"));
    ASSERT_NE(reload, nullptr);
    reload->click();
    settle(3s);
    EXPECT_TRUE(launcher.executables().empty()) << "launches before trust:" << launcher.describe();
    EXPECT_FALSE(rig.trust->isTrusted(dir.path())) << "trusted silently";
    EXPECT_FALSE(frost::io::realFileSystem().exists(dir.join("cmake-build-debug")).value_or(true))
        << "a build directory before trust";

    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr) << "opening an untrusted folder offers trust";
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(waitFor([&] { return launcher.count({"cmake"}) > 0; }, 120s))
        << "the trust grant did not start the configure; launches:" << launcher.describe();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, 240s));
    ASSERT_TRUE(rig.workbench->cmakeService()->waitForIdle(120s));
}

/*
 * An untrusted FrostEngine project folder (a .frostproj beside a CMakeLists.txt). Opening it and
 * pressing every FrostEngine action starts no engine command, no build and no configure, and trusts
 * nothing; each action says to trust the project. After the trust dialog's grant, Play starts
 * exactly one engine helper (scripted).
 */
TEST(R0Trust, AnUntrustedFrostProjFolderRunsNoEngineCommandBuildOrConfigureUntilTrustIsGranted) {
    WorkDir profile("untrusted-engine-profile");
    WorkDir dir("untrusted-engine");
    writeCMakeProject(dir);
    dir.write("Game.frostproj", "{}");
    dir.write("Assets/Scripts/enemy.lua", "return {}\n");
    const std::string helper = QCoreApplication::applicationFilePath().toStdString();
    frost::io::ScriptedProcessLauncher scripted;
    RecordingLauncher launcher(helper, scripted);
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(profile, options);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Scripts/enemy.lua")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    for (const char* action :
         {"Play", "Stop", "State", "Validate Script", "Run Tests", "Refresh Log", "Focus Editor"}) {
        auto* pressed = button(panel, QString::fromLatin1(action));
        ASSERT_NE(pressed, nullptr) << action;
        pressed->click();
        EXPECT_TRUE(hasStatus(panel, QStringLiteral("Trust the project"))) << action;
    }
    ASSERT_TRUE(rig.workbench->build().build(frost::build::BuildAction::Build).has_value());
    settle(3s);
    EXPECT_TRUE(launcher.executables().empty()) << "launches before trust:" << launcher.describe();
    EXPECT_TRUE(scripted.launches().empty());
    EXPECT_FALSE(rig.trust->isTrusted(dir.path())) << "trusted silently";

    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr) << "opening an untrusted FrostEngine project offers trust";
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    // Wait out what the grant itself starts (the build asked for above, its configure), then play.
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, 240s));
    if (auto* cmake = rig.workbench->cmakeService())
        ASSERT_TRUE(cmake->waitForIdle(120s));
    const auto reply = frost::lang::Json::object()
                           .set("ok", true)
                           .set("value", frost::lang::Json::object()
                                             .set("result", frost::lang::Json::object().set("state", "Play"))
                                             .set("locations", frost::lang::Json::object()));
    scripted.expect(frost::io::ScriptedProcess{.stdoutBytes = reply.dump()});
    button(panel, QStringLiteral("Play"))->click();
    ASSERT_TRUE(waitFor([&] { return hasStatus(panel, QStringLiteral("FrostEngine request complete")); }, 10s));
    ASSERT_EQ(scripted.launches().size(), 1U);
    EXPECT_EQ(frost::lang::parseJson(scripted.launches().front().stdinBytes)->stringAt("method"), "play.start");
}

// The pinned LuaLS (ADR 0079); a missing one fails the test (LUA-1).
frost::Result<std::string> pinnedLuaLs() {
    auto located = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!located)
        return frost::fail(frost::ErrorCode::NotFound,
                           "the pinned LuaLS " + std::string(frost::lang::pinnedLuaLanguageServerVersion()) +
                               " is required (tools/bootstrap): " + located.error().describe());
    return located;
}

/*
 * R0 follow-up 13: FrostEngine engine helpers beside a running LuaLS for the same .frostproj
 * folder. The real pinned LuaLS starts for the trusted project and reports an undefined global in
 * a script under Assets; then State, Validate Script (with that open script's buffer) and Play each
 * start one scripted engine helper and show its answer. Every launch is accounted for: exactly one
 * LuaLS and exactly three helpers, in that order; the LuaLS stays Running throughout and still
 * answers afterwards (a fresh edit gets fresh diagnostics).
 */
TEST(R0Engine, EngineHelpersRunBesideARunningLuaLsForTheSameFrostProjFolder) {
    auto luals = pinnedLuaLs();
    ASSERT_TRUE(luals.has_value()) << luals.error().describe();
    WorkDir profile("engine-luals-profile");
    WorkDir dir("engine-luals");
    dir.write("Game.frostproj", "{}");
    dir.write("Assets/Scripts/enemy.lua", "local speed = missingSpeed\nreturn speed\n");
    const std::string script = dir.join("Assets/Scripts/enemy.lua");
    const std::string helper = QCoreApplication::applicationFilePath().toStdString();
    frost::io::ScriptedProcessLauncher scripted;
    RecordingLauncher launcher(helper, scripted);
    RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(profile, options);
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  *luals));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_TRUE(rig.workbench->openPath(script));
    const auto running = [&] {
        return rig.workbench->lua() && rig.workbench->lua()->state() == frost::lang::LuaState::Running;
    };
    ASSERT_TRUE(waitFor(running, 60s)) << "the pinned LuaLS did not start; launches:" << launcher.describe();
    auto* lua = rig.workbench->lua();
    const auto reports = [&](std::string_view name) {
        return waitFor(
            [&] {
                auto set = lua->diagnostics().forFile(script);
                return set && std::ranges::any_of(set->diagnostics, [&](const auto& d) {
                           return d.code == "undefined-global" && d.message.find(name) != std::string::npos;
                       });
            },
            60s);
    };
    ASSERT_TRUE(reports("missingSpeed")) << "LuaLS reported no undefined global";
    ASSERT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();

    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* output = panel->findChild<QListWidget*>();
    ASSERT_NE(output, nullptr);
    const auto answer = [](frost::lang::Json result) {
        return frost::lang::Json::object()
            .set("ok", true)
            .set("value", frost::lang::Json::object()
                              .set("result", std::move(result))
                              .set("locations", frost::lang::Json::object()))
            .dump();
    };
    struct Step {
        const char* action;
        const char* method;
        std::string reply;
        QString shown;
    };
    const std::vector<Step> steps = {
        {"State", "play.state", answer(frost::lang::Json::object().set("state", "Edit")), QStringLiteral("Edit")},
        {"Validate Script", "script.validate", answer(frost::lang::Json::object().set("valid", true)), QString()},
        {"Play", "play.start", answer(frost::lang::Json::object().set("state", "Play")), QStringLiteral("Play")},
    };
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const Step& step = steps[i];
        scripted.expect(frost::io::ScriptedProcess{.stdoutBytes = step.reply});
        ASSERT_TRUE(rig.workbench->openPath(script)); // Validate Script needs the script in front.
        auto* pressed = button(panel, QString::fromLatin1(step.action));
        ASSERT_NE(pressed, nullptr) << step.action;
        pressed->click();
        const QString done = std::string_view(step.method) == "script.validate" ? QStringLiteral("Script is valid")
                                                                                : QStringLiteral("request complete");
        ASSERT_TRUE(waitFor([&] { return hasStatus(panel, done); }, 20s)) << step.action;
        if (!step.shown.isEmpty()) {
            ASSERT_GT(output->count(), 0) << step.action;
            EXPECT_EQ(output->item(output->count() - 1)->text(), step.shown) << step.action;
        }
        const auto launched = scripted.launches();
        ASSERT_EQ(launched.size(), i + 1) << step.action;
        auto request = frost::lang::parseJson(launched.back().stdinBytes);
        ASSERT_TRUE(request.has_value()) << step.action;
        EXPECT_EQ(request->stringAt("method"), step.method);
        EXPECT_TRUE(running()) << "LuaLS left Running during " << step.action;
    }
    // The validated buffer is the open script's text, sent to the engine helper, not to LuaLS.
    auto validate = frost::lang::parseJson(scripted.launches()[1].stdinBytes);
    ASSERT_TRUE(validate.has_value());
    EXPECT_NE(validate->dump().find("missingSpeed"), std::string::npos) << validate->dump();

    // The spawn log: one LuaLS, then exactly the three helpers, and nothing else of either kind.
    const auto all = launcher.executables();
    const std::string& helperExe = helper;
    EXPECT_EQ(std::ranges::count(all, helperExe), 3) << launcher.describe();
    EXPECT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();
    const auto firstHelper = std::ranges::find(all, helperExe);
    const auto lualsLaunch =
        std::ranges::find_if(all, [](const std::string& e) { return toolName(e) == "lua-language-server"; });
    EXPECT_LT(lualsLaunch - all.begin(), firstHelper - all.begin()) << launcher.describe();

    // LuaLS still answers: a new undefined global in an edit is reported, by the same process.
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(view->path(), script);
    view->editor().editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{0}));
    ASSERT_TRUE(view->editor().typeText("local boost = missingBoost\n"));
    ASSERT_TRUE(reports("missingBoost")) << "LuaLS stopped answering after the engine commands";
    EXPECT_TRUE(running());
    EXPECT_EQ(rig.workbench->lua(), lua) << "the LuaLS was replaced";
    EXPECT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();
}

} // namespace
