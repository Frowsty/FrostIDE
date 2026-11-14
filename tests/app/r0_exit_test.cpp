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
//   - R0 follow-up 13: a labelled HMAC-v2 engine session peer while the real pinned LuaLS runs
//     for the same .frostproj folder. Requests and pushed state coexist with real Lua diagnostics,
//     every child launch is accounted for, and the same LuaLS stays Running.
// The profile data (settings, trust store, sessions) of every rig lives in a work directory of its
// own, apart from the project, so a "nothing was written" check sees only what the product wrote.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/rpc/json.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/engine_console.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "engine_session_peer.hpp"
#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QCoreApplication>
#include <QLabel>
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
 * The spawn log of a test: every launch's executable, in order, forwarded to the actual launcher.
 * The engine protocol peer opens a local socket and does not create a child process. Thread-safe.
 */
class RecordingLauncher final : public frost::io::IProcessLauncher {
public:
    RecordingLauncher() = default;

    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        record(spec);
        if (auto refused = refuseThisBinary(spec); !refused)
            return std::unexpected(std::move(refused.error()));
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        record(spec);
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
    // Refuse a mistaken recursive launch of this test binary.
    static frost::Result<void> refuseThisBinary(const frost::io::ProcessSpec& spec) {
        const std::string self = QCoreApplication::applicationFilePath().toStdString();
        if (frost::io::normalizePath(spec.executable).value_or(spec.executable) ==
            frost::io::normalizePath(self).value_or(self))
            return frost::fail(frost::ErrorCode::InvalidArgument, "the test refuses to start its own binary");
        return {};
    }
    void record(const frost::io::ProcessSpec& spec) {
        const std::scoped_lock lock(m_mutex);
        m_executables.push_back(spec.executable);
    }
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
    rig.window->toolWindows().show(frost::app::kUnitTestsToolWindow);
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

using EngineSessionPeer = frost::app::test::EngineSessionPeer;
using EngineJson = frost::rpc::Json;

void advertiseEnginePeer(const WorkDir& profile, const EngineSessionPeer& peer) {
    profile.write("automation/Instances/" + peer.instanceId() + ".json", peer.record().dump());
}

bool pairEnginePeer(WorkbenchRig& rig, const EngineSessionPeer& peer) {
    EXPECT_NE(peer.port(), 0U);
    auto refreshed = rig.workbench->refreshEngineInstances();
    EXPECT_TRUE(refreshed) << (refreshed ? "" : refreshed.error().describe());
    if (!refreshed || !waitFor(
                          [&] {
                              const auto state = rig.workbench->engineState();
                              const auto* instances = state.find("instances");
                              return instances && std::ranges::any_of(instances->items(), [&](const auto& instance) {
                                         return instance.stringAt("instanceId") == peer.instanceId();
                                     });
                          },
                          5s))
        return false;
    auto paired = rig.workbench->pairEngineInstance(peer.instanceId());
    EXPECT_TRUE(paired) << (paired ? "" : paired.error().describe());
    return paired && waitFor(
                         [&] {
                             const auto state = rig.workbench->engineState();
                             return state.boolAt("connected") == true &&
                                    state.stringAt("selectedInstanceId") == peer.instanceId() &&
                                    state.stringAt("state") == "Edit";
                         },
                         5s);
}

bool engineCommandCompleted(WorkbenchRig& rig, std::uint64_t request) {
    const auto state = rig.workbench->engineState();
    const auto* reply = state.find("lastReply");
    return state.unsignedAt("lastCompletedRequestId") == request && reply && !reply->isNull();
}

bool consoleContains(frost::ui::EngineConsole& console, const QString& text) {
    for (QTreeWidgetItemIterator it(console.view()); *it; ++it)
        if ((*it)->text(3).contains(text))
            return true;
    return false;
}

// This is a protocol fixture reply, independent from the explicit state event below.
EngineJson peerPlayReceipt() {
    return EngineJson::object()
        .set("state", "Edit")
        .set("mode", "Edit")
        .set("paused", false)
        .set("frames", 0)
        .set("accepted", true)
        .set("pending", true)
        .set("startedNow", false)
        .set("operationId", std::string(32, 'd'))
        .set("phase", "pending")
        .set("playGeneration", "");
}
void publishPeerPlay(EngineSessionPeer& peer) {
    peer.event("play.state", EngineJson::object()
                                 .set("state", "Play")
                                 .set("mode", "Play")
                                 .set("paused", false)
                                 .set("frames", 7)
                                 .set("operationId", std::string(32, 'd'))
                                 .set("phase", "playing")
                                 .set("playGeneration", std::string(32, 'e')));
}

void exercisePeerPlay(WorkbenchRig& rig, EngineSessionPeer& peer) {
    const auto receipt = peerPlayReceipt();
    peer.result("play.start", receipt);
    auto* play = rig.window->mainToolbar().button(frost::ui::MainToolbar::kEnginePlay);
    ASSERT_NE(play, nullptr);
    ASSERT_TRUE(play->isEnabled());
    play->click();
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.start").size() == 1; }, 10s));
    const auto request = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(request);
    ASSERT_TRUE(waitFor([&] { return engineCommandCompleted(rig, *request); }, 20s));
    const auto replied = rig.workbench->engineState();
    ASSERT_NE(replied.find("lastReply"), nullptr);
    EXPECT_EQ(*replied.find("lastReply"), receipt);
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* console = panel->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    ASSERT_TRUE(waitFor([&] { return consoleContains(*console, QStringLiteral("play.start")); }, 20s));
    EXPECT_EQ(replied.stringAt("state"), "Edit");
    EXPECT_TRUE(replied.stringAt("playGeneration").empty());
    ASSERT_TRUE(rig.window->mainToolbar().engineState());
    EXPECT_FALSE(rig.window->mainToolbar().engineState()->playing);
    const auto sequence = replied.unsignedAt("eventSequence").value_or(0);
    publishPeerPlay(peer);
    ASSERT_TRUE(waitFor(
        [&] {
            const auto state = rig.workbench->engineState();
            return state.unsignedAt("eventSequence").value_or(0) > sequence && state.stringAt("state") == "Play" &&
                   state.boolAt("playing") == true &&
                   state.stringAt("operationId") == receipt.stringAt("operationId") &&
                   state.stringAt("phase") == "playing" && state.stringAt("playGeneration") == std::string(32, 'e');
        },
        10s));
    EXPECT_TRUE(rig.window->mainToolbar().engineState()->playing);
}

/*
 * Untrusted project entry points send no engine frames and start no children. The actual trust
 * dialog grant enables pairing with the labelled HMAC-v2 protocol peer and the normal Play action.
 * CMake remains an actual installed tool, observed by the same recording launcher.
 */
TEST(R0Trust, AnUntrustedFrostProjFolderRunsNoEngineCommandBuildOrConfigureUntilTrustIsGranted) {
    WorkDir profile("untrusted-engine-profile");
    WorkDir dir("untrusted-engine");
    writeCMakeProject(dir);
    dir.write("Game.frostproj", "{}");
    dir.write("Assets/Scripts/enemy.lua", "return {}\n");
    EngineSessionPeer peer(dir.path());
    ASSERT_NE(peer.port(), 0U);
    advertiseEnginePeer(profile, peer);
    RecordingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.engineAutomationDirectory = profile.join("automation");
    WorkbenchRig rig(profile, options);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Game.frostproj")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Assets/Scripts/enemy.lua")));
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* console = panel->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    EXPECT_FALSE(console->submitCommand(QStringLiteral("play.start")));
    auto paired = rig.workbench->pairEngineInstance(peer.instanceId());
    EXPECT_FALSE(paired);
    EXPECT_FALSE(rig.workbench->validateEngineScript());
    EXPECT_FALSE(rig.workbench->startEngineEditor());
    for (const char* method : {"play.start", "play.stop", "play.state", "play.pause", "play.step", "play.simulate",
                               "script.validate", "test.run", "editor.focus"}) {
        auto denied = rig.workbench->sendEngineCommand(method, EngineJson::object());
        ASSERT_FALSE(denied) << method;
        EXPECT_FALSE(denied.error().message().empty()) << method;
        EXPECT_TRUE(peer.requests(method).empty()) << method;
    }
    for (const auto id : {frost::ui::MainToolbar::kEnginePlay, frost::ui::MainToolbar::kEngineStop,
                          frost::ui::MainToolbar::kEnginePause, frost::ui::MainToolbar::kEngineStep,
                          frost::ui::MainToolbar::kEngineSimulate}) {
        auto* action = rig.window->mainToolbar().button(id);
        ASSERT_NE(action, nullptr);
        EXPECT_FALSE(action->isEnabled());
        action->click();
    }
    ASSERT_TRUE(rig.workbench->build().build(frost::build::BuildAction::Build));
    settle(3s);
    EXPECT_TRUE(launcher.executables().empty()) << "launches before trust:" << launcher.describe();
    EXPECT_EQ(peer.authenticated.load(), 0U);
    EXPECT_TRUE(peer.requests("auth.challenge").empty());
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    EXPECT_EQ(rig.workbench->engineState().boolAt("connected"), false);
    const auto beforeTrust = rig.workbench->engineState();
    ASSERT_NE(beforeTrust.find("lastReply"), nullptr);
    EXPECT_TRUE(beforeTrust.find("lastReply")->isNull());

    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr);
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->build().isBuilding(); }, 240s));
    if (auto* cmake = rig.workbench->cmakeService())
        ASSERT_TRUE(cmake->waitForIdle(120s));
    ASSERT_TRUE(pairEnginePeer(rig, peer)) << allNotices(rig);
    EXPECT_GT(peer.authenticated.load(), 0U);
    const auto beforeCommands = launcher.executables();
    ASSERT_NO_FATAL_FAILURE(exercisePeerPlay(rig, peer));
    EXPECT_EQ(launcher.executables(), beforeCommands) << launcher.describe();
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
 * R0 follow-up 13 through the persistent session transport. A labelled HMAC-v2 engine peer
 * receives state/validation/Play requests while the actual pinned LuaLS diagnoses the same
 * unsaved script. Replies and pushes are observed separately, every child belongs to the real
 * LuaLS, and that same server still reports a fresh undefined global afterwards.
 */
TEST(R0Engine, PersistentSessionPeerRunsBesideARunningLuaLsForTheSameFrostProjFolder) {
    auto luals = pinnedLuaLs();
    ASSERT_TRUE(luals) << luals.error().describe();
    WorkDir profile("engine-luals-profile");
    WorkDir dir("engine-luals");
    dir.write("Game.frostproj", "{}");
    const std::string diskSource = "local speed = missingSpeed\nreturn speed\n";
    dir.write("Assets/Scripts/enemy.lua", diskSource);
    const std::string script = dir.join("Assets/Scripts/enemy.lua");
    EngineSessionPeer peer(dir.path());
    ASSERT_NE(peer.port(), 0U);
    advertiseEnginePeer(profile, peer);
    RecordingLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.workbench.engineAutomationDirectory = profile.join("automation");
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
                return set && std::ranges::any_of(set->diagnostics, [&](const auto& diagnostic) {
                           return diagnostic.code == "undefined-global" &&
                                  diagnostic.message.find(name) != std::string::npos;
                       });
            },
            60s);
    };
    ASSERT_TRUE(reports("missingSpeed"));
    ASSERT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();
    ASSERT_TRUE(pairEnginePeer(rig, peer));
    const auto connectionGeneration = rig.workbench->engineState().unsignedAt("connectionGeneration");
    ASSERT_TRUE(connectionGeneration && *connectionGeneration > 0);
    rig.window->toolWindows().show("frostEngine");
    auto* panel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(panel, nullptr);
    auto* console = panel->findChild<frost::ui::EngineConsole*>(QStringLiteral("engineConsole"));
    ASSERT_NE(console, nullptr);
    const auto launchesBeforeCommands = launcher.executables();

    const auto edit =
        EngineJson::object().set("state", "Edit").set("mode", "Edit").set("paused", false).set("frames", 0);
    peer.result("play.state", edit);
    ASSERT_TRUE(console->submitCommand(QStringLiteral("play.state")));
    ASSERT_TRUE(waitFor([&] { return peer.requests("play.state").size() == 1; }, 20s));
    const auto stateRequest = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(stateRequest);
    ASSERT_TRUE(waitFor([&] { return engineCommandCompleted(rig, *stateRequest); }, 20s));
    const auto stateReply = rig.workbench->engineState();
    ASSERT_NE(stateReply.find("lastReply"), nullptr);
    EXPECT_EQ(*stateReply.find("lastReply"), edit);
    ASSERT_TRUE(waitFor([&] { return consoleContains(*console, QStringLiteral("Edit")); }, 20s));
    EXPECT_TRUE(running());

    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(view->path(), script);
    view->editor().editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{0}));
    ASSERT_TRUE(view->editor().typeText("-- validation buffer only\n"));
    const auto submitted = view->editor().document().text().text();
    ASSERT_NE(submitted, diskSource);
    peer.hold("script.validate");
    auto* validate = panel->findChild<QPushButton*>(QStringLiteral("engineValidateScriptButton"));
    ASSERT_NE(validate, nullptr);
    ASSERT_TRUE(validate->isEnabled());
    validate->click();
    ASSERT_TRUE(waitFor([&] { return peer.requests("script.validate").size() == 1; }, 20s));
    const auto validationId = rig.workbench->engineState().unsignedAt("lastRequestId");
    ASSERT_TRUE(validationId);
    ASSERT_TRUE(view->editor().typeText("-- changed after submission\n"));
    const auto requests = peer.requests("script.validate");
    const auto* params = requests.front().find("params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->stringAt("path"), "Assets/Scripts/enemy.lua");
    EXPECT_EQ(params->stringAt("source"), submitted);
    EXPECT_NE(view->editor().document().text().text(), submitted);
    const auto validationReply = EngineJson::object().set("valid", true).set("diagnostics", EngineJson::array());
    peer.release("script.validate", validationReply);
    ASSERT_TRUE(waitFor([&] { return engineCommandCompleted(rig, *validationId); }, 20s));
    const auto validated = rig.workbench->engineState();
    ASSERT_NE(validated.find("lastReply"), nullptr);
    EXPECT_EQ(*validated.find("lastReply"), validationReply);
    ASSERT_TRUE(waitFor([&] { return consoleContains(*console, QStringLiteral("script.validate")); }, 20s));
    EXPECT_TRUE(consoleContains(*console, QStringLiteral("valid")));
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(dir.read("Assets/Scripts/enemy.lua"), diskSource);
    EXPECT_TRUE(running());

    ASSERT_NO_FATAL_FAILURE(exercisePeerPlay(rig, peer));
    EXPECT_TRUE(running());
    EXPECT_EQ(launcher.executables(), launchesBeforeCommands) << launcher.describe();
    EXPECT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();
    EXPECT_EQ(launcher.count(kBuildTools), 0U) << launcher.describe();
    EXPECT_EQ(launcher.executables().size(), launcher.count({"lua-language-server"})) << launcher.describe();

    view->editor().editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{0}));
    ASSERT_TRUE(view->editor().typeText("local boost = missingBoost\n"));
    ASSERT_TRUE(reports("missingBoost")) << "LuaLS stopped answering after session commands";
    EXPECT_TRUE(running());
    EXPECT_EQ(rig.workbench->lua(), lua);
    EXPECT_EQ(rig.workbench->engineState().unsignedAt("connectionGeneration"), connectionGeneration);
    EXPECT_EQ(peer.authenticated.load(), 1U);
    EXPECT_EQ(launcher.count({"lua-language-server"}), 1U) << launcher.describe();
    EXPECT_EQ(launcher.executables(), launchesBeforeCommands) << launcher.describe();
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(dir.read("Assets/Scripts/enemy.lua"), diskSource);
}

} // namespace
