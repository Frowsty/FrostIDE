// SPDX-License-Identifier: Apache-2.0
// Mandatory corpus R2a Workbench debugger acceptance on an owned copy of pinned Notepad++.
#include "frost/build/msbuild_locator.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/async_process.hpp"
#include "frost/ui/debug_models.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/private_desktop.hpp"
#include "workbench_rig.hpp"
#include <QCoreApplication>
#include <QLineEdit>
#include <QScopeGuard>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

struct ProcessInfo {
    DWORD pid;
    DWORD parent;
    std::wstring path;
    FILETIME created;
};
std::vector<ProcessInfo> processes() {
    std::vector<ProcessInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry))
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768];
            DWORD length = static_cast<DWORD>(std::size(path));
            FILETIME created{}, exited{}, kernel{}, user{};
            if (QueryFullProcessImageNameW(process, 0, path, &length) &&
                GetProcessTimes(process, &created, &exited, &kernel, &user))
                result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, std::wstring(path, length), created});
            CloseHandle(process);
        } while (Process32NextW(snap, &entry));
    CloseHandle(snap);
    return result;
}
struct OwnedProcesses {
    std::vector<HANDLE> handles;
    HANDLE adopt(const ProcessInfo& identity) {
        HANDLE process =
            OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid);
        if (process == nullptr)
            return nullptr;
        wchar_t image[32768];
        DWORD length = static_cast<DWORD>(std::size(image));
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!QueryFullProcessImageNameW(process, 0, image, &length) ||
            !GetProcessTimes(process, &created, &exited, &kernel, &user) ||
            CompareFileTime(&created, &identity.created) != 0 ||
            QString::fromWCharArray(image, static_cast<qsizetype>(length))
                    .compare(QString::fromStdWString(identity.path), Qt::CaseInsensitive) != 0) {
            CloseHandle(process);
            return nullptr;
        }
        handles.push_back(process);
        return process;
    }
    ~OwnedProcesses() {
        for (HANDLE process : handles) {
            DWORD status = WaitForSingleObject(process, 5000);
            if (status == WAIT_TIMEOUT) {
                EXPECT_NE(TerminateProcess(process, 99), FALSE); // Only the identified acceptance child.
                status = WaitForSingleObject(process, 5000);
            }
            EXPECT_EQ(status, WAIT_OBJECT_0) << "owned native process cleanup did not finish";
            EXPECT_NE(CloseHandle(process), FALSE);
        }
    }
};
struct RemoveUserFile {
    std::filesystem::path path;
    ~RemoveUserFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
struct RemoveCreatedWorkspace {
    std::filesystem::path dir;
    ~RemoveCreatedWorkspace() {
        std::error_code error;
        std::filesystem::remove(dir / L"workspace.json", error);
        std::filesystem::remove(dir / L".gitignore", error);
        std::filesystem::remove(dir, error); // Never recursively remove unexpected files.
    }
};

bool visibleWindow(DWORD pid, HDESK desktop) {
    struct Search {
        DWORD pid;
        bool found = false;
    } search{pid};
    EnumDesktopWindows(
        desktop,
        [](HWND hwnd, LPARAM value) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(value);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == search.pid && IsWindowVisible(hwnd)) {
                search.found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

class DesktopLauncher final : public frost::io::IProcessLauncher {
public:
    explicit DesktopLauncher(std::string desktop) : m_desktop(std::move(desktop)) {}
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        auto isolated = spec;
        isolated.desktop = m_desktop;
        return frost::io::realProcessLauncher().launch(isolated);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        auto isolated = spec;
        isolated.desktop = m_desktop;
        return frost::io::realProcessLauncher().launchAsync(isolated, context);
    }

private:
    std::string m_desktop;
};

// Fixture preparation precedes the unchanged F5 observation deadline. Normal Build executes
// Notepad++'s own generated-version-header prebuild event, which selected-file ClCompile omits.
frost::Result<frost::io::ProcessResult> fixtureProcess(const std::string& executable,
                                                       std::vector<std::string> arguments, const std::string& directory,
                                                       std::chrono::seconds timeout = 120s) {
    frost::io::ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    spec.environment = {{"GIT_DIR", std::nullopt}, {"GIT_WORK_TREE", std::nullopt}, {"GIT_INDEX_FILE", std::nullopt}};
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, timeout);
    if (!result)
        return std::unexpected(result.error());
    if (result->exitCode != 0) {
        return frost::fail(frost::ErrorCode::IoError, "R2a fixture command failed: " + executable + "\n" +
                                                          result->stdoutBytes + result->stderrBytes);
    }
    return result;
}

TEST(WorkbenchR2a, NotepadPlusPlusDebuggerAcceptance) {
    WorkDir fixture("r2a-debug-fixture");
    auto& fs = frost::io::realFileSystem();
    auto git = frost::vcs::findGit(fs, frost::vcs::defaultGitSearch());
    ASSERT_TRUE(git) << git.error().describe();
    std::string inputRoot = qEnvironmentVariable("FROST_R2A_ROOT").toStdString();
    if (inputRoot.empty()) {
        auto located = fixtureProcess(FROST_TEST_PYTHON, {FROST_R2A_CORPORA_TOOL, "path", "r2a"}, fixture.path());
        ASSERT_TRUE(located) << located.error().describe();
        inputRoot = QString::fromStdString(located->stdoutBytes).trimmed().toStdString();
    }
    auto sourceRoot = frost::io::normalizePath(inputRoot);
    ASSERT_TRUE(sourceRoot) << sourceRoot.error().describe();
    auto head = fixtureProcess(git->path, {"-C", *sourceRoot, "rev-parse", "HEAD"}, fixture.path());
    ASSERT_TRUE(head) << head.error().describe();
    ASSERT_EQ(QString::fromStdString(head->stdoutBytes).trimmed().toStdString(), FROST_CORPUS_R2A_SHA);
    auto clean =
        fixtureProcess(git->path, {"-C", *sourceRoot, "status", "--porcelain", "--untracked-files=no"}, fixture.path());
    ASSERT_TRUE(clean) << clean.error().describe();
    ASSERT_TRUE(clean->stdoutBytes.empty()) << "the pinned source has tracked edits";
    const std::string root = fixture.join("source");
    auto cloned =
        fixtureProcess(git->path, {"clone", "--no-hardlinks", "--no-checkout", *sourceRoot, root}, fixture.path());
    ASSERT_TRUE(cloned) << cloned.error().describe();
    auto checkedOut =
        fixtureProcess(git->path, {"-C", root, "checkout", "--detach", FROST_CORPUS_R2A_SHA}, fixture.path());
    ASSERT_TRUE(checkedOut) << checkedOut.error().describe();
    const auto trackedSourceUnchanged = qScopeGuard([&] {
        auto status =
            fixtureProcess(git->path, {"-C", root, "status", "--porcelain", "--untracked-files=no"}, fixture.path());
        EXPECT_TRUE(status) << status.error().describe();
        if (status)
            EXPECT_TRUE(status->stdoutBytes.empty()) << status->stdoutBytes;
    });
    std::string scratch = qEnvironmentVariable("FROST_R2A_PROFILE").toStdString();
    if (scratch.empty())
        scratch = fixture.join("profile");
    auto normalizedProfile = frost::io::normalizePath(scratch);
    ASSERT_TRUE(normalizedProfile) << normalizedProfile.error().describe();
    scratch = *normalizedProfile;
    ASSERT_FALSE(fs.exists(scratch + "/settings").value_or(true)) << "R2a settings must be a fresh test directory";
    for (const std::string_view directory : {"settings", "appdata", "localappdata", "userprofile"})
        ASSERT_TRUE(fs.createDirectories(scratch + "/" + std::string(directory)).has_value());
    const std::string project = root + "/PowerEditor/visual.net/notepadPlus.vcxproj";
    const std::string solution = root + "/PowerEditor/visual.net/notepadPlus.sln";
    const std::string source = root + "/PowerEditor/src/winmain.cpp";
    const std::string executable = root + "/PowerEditor/visual.net/x64/Debug/notepad++.exe";
    const std::string pdb = root + "/PowerEditor/visual.net/x64/Debug/notepadPlus.pdb";
    auto msbuild = frost::build::findMsBuild(frost::io::realProcessLauncher(), fs);
    ASSERT_TRUE(msbuild) << msbuild.error().describe();
    auto built = fixtureProcess(msbuild->msbuildPath,
                                {solution, "-t:Build", "-p:Configuration=Debug", "-p:Platform=x64",
                                 "-p:VCToolsVersion=" FROST_PINNED_VC_TOOLS,
                                 "-p:WindowsTargetPlatformVersion=" FROST_PINNED_WINSDK},
                                root + "/PowerEditor/visual.net", 1h);
    ASSERT_TRUE(built) << built.error().describe();
    RecordProperty("r2aSourceSha", FROST_CORPUS_R2A_SHA);
    RecordProperty("r2aFixtureBuildOutput", built->stdoutBytes + built->stderrBytes);
    ASSERT_TRUE(fs.exists(executable).value_or(false)) << executable;
    ASSERT_TRUE(fs.exists(pdb).value_or(false)) << pdb;
    ASSERT_TRUE(fs.exists(root + "/PowerEditor/src/NppLibsVersion.h").value_or(false));
    const std::string userFile = project + ".user";
    ASSERT_FALSE(fs.exists(userFile).value_or(true)) << "refusing to overwrite a pre-existing user file";
    RemoveUserFile cleanup{std::filesystem::path(QString::fromStdString(userFile).toStdWString())};
    const std::string workspace = root + "/PowerEditor/visual.net/.frost";
    ASSERT_FALSE(fs.exists(workspace).value_or(true)) << "refusing to alter an existing project workspace";
    RemoveCreatedWorkspace workspaceCleanup{std::filesystem::path(QString::fromStdString(workspace).toStdWString())};
    std::string arguments = "-multiInst -nosession -noPlugin ";
    frost::io::appendWindowsArgument(arguments, "-settingsDir=" + scratch + "/settings");
    const std::string xmlArguments = QString::fromStdString(arguments).toHtmlEscaped().toStdString();
    const std::string environment = "APPDATA=" + scratch + "/appdata\nLOCALAPPDATA=" + scratch +
                                    "/localappdata\nUSERPROFILE=" + scratch + "/userprofile";
    const std::string xmlEnvironment = QString::fromStdString(environment).toHtmlEscaped().toStdString();
    const std::string user =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><Project "
        "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"><PropertyGroup "
        "Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\"><LocalDebuggerCommandArguments>" +
        xmlArguments + "</LocalDebuggerCommandArguments><LocalDebuggerEnvironment>" + xmlEnvironment +
        "</LocalDebuggerEnvironment></PropertyGroup></Project>";
    ASSERT_TRUE(fs.writeFileAtomic(userFile, user).has_value());
    frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    DesktopLauncher launcher(desktop.name());
    const auto before = processes();
    OwnedProcesses owned;
    WorkDir work("r2a-debug");
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    WorkbenchRig rig(work, std::move(options));
    ASSERT_TRUE(rig.trust->trust(root).has_value());
    ASSERT_TRUE(rig.workbench->openPath(solution).has_value());
    const auto model = rig.workbench->solution();
    ASSERT_TRUE(model);
    const auto config = model->findConfiguration("Debug|x64");
    ASSERT_TRUE(config);
    ASSERT_TRUE(rig.workbench->build().setActiveConfiguration(*config).has_value());
    const auto startup = model->findProjectByPath(project);
    ASSERT_TRUE(startup);
    ASSERT_TRUE(rig.workbench->runService()->setStartupProject(*startup).has_value());
    auto* debug = rig.workbench->debugController();
    if (!debug->breakpoints().at(source, 641))
        debug->toggleBreakpoint(source, 640);
    int stops = 0;
    QObject::connect(debug, &frost::ui::DebugController::stopped, [&] { ++stops; });
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return debug->paused() && debug->selectedFrame().has_value(); }, 240s))
        << rig.workbench->runConsole()->text().toStdString();
    auto findChild = [&]() -> std::optional<ProcessInfo> {
        for (const auto& process : processes()) {
            if (QString::fromStdWString(process.path)
                    .compare(QString::fromStdString(executable).replace('/', '\\'), Qt::CaseInsensitive) != 0)
                continue;
            if (std::ranges::none_of(before, [&](const auto& old) { return old.pid == process.pid; }))
                return process;
        }
        return std::nullopt;
    };
    ASSERT_TRUE(waitFor([&] { return findChild().has_value(); }, 30s));
    const auto child = *findChild();
    HANDLE process = owned.adopt(child);
    ASSERT_NE(process, nullptr);
    const auto running = processes();
    const auto parent = std::ranges::find_if(running, [&](const auto& item) { return item.pid == child.parent; });
    ASSERT_NE(parent, running.end());
    ASSERT_TRUE(QString::fromStdWString(parent->path).endsWith(QStringLiteral("lldb-dap.exe"), Qt::CaseInsensitive));
    ASSERT_TRUE(std::ranges::none_of(before, [&](const auto& old) { return old.pid == parent->pid; }));
    HANDLE adapter = owned.adopt(*parent);
    ASSERT_NE(adapter, nullptr);
    std::cout << "R2A debuggee PID=" << child.pid << " lldb-dap PID=" << parent->pid << '\n' << std::flush;
    ASSERT_TRUE(debug->breakpoints().at(source, 641));
    EXPECT_EQ(debug->breakpoints().at(source, 641)->status, frost::debug::BreakpointStatus::Verified);
    EXPECT_EQ(debug->selectedFrame()->line, 641);
    EXPECT_NE(debug->selectedFrame()->name.find("wWinMain"), std::string::npos);
    std::cout << "R2A verified stop at wWinMain line=" << debug->selectedFrame()->line << '\n' << std::flush;
    auto* window = rig.window->findChild<frost::ui::DebugToolWindow*>();
    ASSERT_NE(window, nullptr);
    ASSERT_GE(window->framesModel()->rowCount(), 2);
    ASSERT_TRUE(std::ranges::any_of(debug->frames().frames, [](const auto& frame) {
        return frame.name.find("invoke_main") != std::string::npos ||
               frame.name.find("__scrt_common_main") != std::string::npos;
    })) << "expected CRT startup frames below wWinMain";
    ASSERT_FALSE(debug->threads().empty());
    auto* variables = window->variablesModel();
    ASSERT_TRUE(waitFor([&] { return variables->rowCount() > 0; }, 60s));
    bool foundCommandLine = false;
    for (int row = 0; row < variables->rowCount(); ++row) {
        const QModelIndex scope = variables->index(row, 0);
        const QString scopeName = variables->data(scope, Qt::DisplayRole).toString();
        std::cout << "R2A scope=" << scopeName.toStdString() << " pending=" << variables->pendingRequests() << '\n'
                  << std::flush;
        if (scopeName != QStringLiteral("Locals") && scopeName != QStringLiteral("Arguments"))
            continue;
        if (variables->canFetchMore(scope))
            variables->fetchMore(scope);
        ASSERT_TRUE(waitFor([&] { return variables->rowCount(scope) > 0 || variables->pendingRequests() == 0; }, 120s))
            << "scope=" << scopeName.toStdString() << " pending=" << variables->pendingRequests();
        for (int child = 0; child < variables->rowCount(scope); ++child) {
            const auto item = variables->index(child, 0, scope);
            if (variables->data(item, Qt::DisplayRole).toString() == QStringLiteral("pCmdLine")) {
                foundCommandLine = true;
                EXPECT_TRUE(variables->valueAt(item.siblingAtColumn(1)).contains(QStringLiteral("multiInst")));
            }
        }
    }
    EXPECT_TRUE(foundCommandLine);
    std::cout << "R2A Locals contained pCmdLine with the requested arguments" << '\n' << std::flush;
    int previous = stops;
    ASSERT_TRUE(debug->stepInto());
    ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    EXPECT_NE(debug->selectedFrame()->name.find("parseCommandLine"), std::string::npos);
    std::cout << "R2A Step Into entered parseCommandLine" << '\n' << std::flush;
    previous = stops;
    ASSERT_TRUE(debug->stepOut());
    ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    EXPECT_NE(debug->selectedFrame()->name.find("wWinMain"), std::string::npos);
    std::cout << "R2A Step Out returned to wWinMain" << '\n' << std::flush;
    for (int count = 0; debug->selectedFrame()->line < 657 && count < 30; ++count) {
        previous = stops;
        ASSERT_TRUE(debug->stepOver());
        ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    }
    ASSERT_EQ(debug->selectedFrame()->line, 657);
    std::cout << "R2A Step Over reached line 657, first executable line after doPrintAndQuit" << '\n' << std::flush;
    auto* watches = window->watchesModel();
    watches->addWatch(QStringLiteral("isMultiInst"));
    ASSERT_TRUE(waitFor([&] { return watches->pendingRequests() == 0; }, 60s));
    EXPECT_EQ(watches->valueAt(watches->index(0, 1)), QStringLiteral("true"));
    frost::ui::EvaluateDialog evaluate(*debug, QStringLiteral("doPrintAndQuit || doFunctionListExport"));
    evaluate.evaluate();
    ASSERT_TRUE(waitFor(
        [&] { return evaluate.resultModel()->rowCount() == 1 && evaluate.resultModel()->pendingRequests() == 0; },
        60s));
    const QString result = evaluate.resultModel()->valueAt(evaluate.resultModel()->index(0, 1));
    std::cout << "R2A watch=" << watches->valueAt(watches->index(0, 1)).toStdString()
              << " evaluate=" << result.toStdString() << '\n'
              << std::flush;
    EXPECT_TRUE(result.endsWith(QStringLiteral("false")));
    ASSERT_TRUE(debug->resume());
    ASSERT_TRUE(waitFor([&] { return visibleWindow(child.pid, desktop.handle()); }, 120s));
    std::cout << "R2A Notepad++ window visible on private desktop" << '\n' << std::flush;
    ASSERT_TRUE(debug->pause());
    previous = stops;
    ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    EXPECT_FALSE(debug->threads().empty());
    const auto pausedThreadCount = debug->threads().size();
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
    EXPECT_EQ(WaitForSingleObject(process, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(WaitForSingleObject(adapter, 10000), WAIT_OBJECT_0);
    EXPECT_TRUE(window->console()->text().contains(QStringLiteral("Process finished")) ||
                window->console()->text().contains(QStringLiteral("Process stopped")));
    std::cout << "R2A pause showed " << pausedThreadCount << " threads; child and adapter stopped" << '\n'
              << std::flush;
}
} // namespace
