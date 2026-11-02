// SPDX-License-Identifier: Apache-2.0
// Explicit, opt-in R2a Workbench debugger acceptance on the pinned Notepad++ corpus.
#include "frost/debug/lldb_dap.hpp"
#include "frost/ui/debug_models.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "workbench_rig.hpp"
#include <QCoreApplication>
#include <QLineEdit>

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
            if (QueryFullProcessImageNameW(process, 0, path, &length))
                result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, std::wstring(path, length)});
            CloseHandle(process);
        } while (Process32NextW(snap, &entry));
    CloseHandle(snap);
    return result;
}
struct OwnedProcesses {
    std::vector<HANDLE> handles;
    ~OwnedProcesses() {
        for (HANDLE process : handles) {
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 99); // Only the new, identified acceptance child.
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
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

bool visibleWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        bool found = false;
    } search{pid};
    EnumWindows(
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

TEST(WorkbenchR2a, NotepadPlusPlusDebuggerAcceptance) {
    const QString corpus = qEnvironmentVariable("FROST_R2A_ROOT");
    const QString profile = qEnvironmentVariable("FROST_R2A_PROFILE");
    if (corpus.isEmpty() || profile.isEmpty())
        GTEST_SKIP() << "set FROST_R2A_ROOT and FROST_R2A_PROFILE to run pinned R2a acceptance";
    const std::string root = frost::io::normalizePath(corpus.toStdString()).value();
    const std::string scratch = frost::io::normalizePath(profile.toStdString()).value();
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch + "/settings").has_value());
    const std::string project = root + "/PowerEditor/visual.net/notepadPlus.vcxproj";
    const std::string solution = root + "/PowerEditor/visual.net/notepadPlus.sln";
    const std::string source = root + "/PowerEditor/src/winmain.cpp";
    const std::string executable = root + "/PowerEditor/visual.net/x64/Debug/notepad++.exe";
    const std::string userFile = project + ".user";
    ASSERT_FALSE(fs.exists(userFile).value_or(true)) << "refusing to overwrite a pre-existing user file";
    RemoveUserFile cleanup{std::filesystem::path(QString::fromStdString(userFile).toStdWString())};
    const std::string workspace = root + "/PowerEditor/visual.net/.frost";
    ASSERT_FALSE(fs.exists(workspace).value_or(true)) << "refusing to alter an existing project workspace";
    RemoveCreatedWorkspace workspaceCleanup{std::filesystem::path(QString::fromStdString(workspace).toStdWString())};
    const std::string arguments = "-multiInst -nosession -noPlugin -settingsDir=" + scratch + "/settings";
    const std::string user =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><Project "
        "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"><PropertyGroup "
        "Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\"><LocalDebuggerCommandArguments>" +
        arguments + "</LocalDebuggerCommandArguments></PropertyGroup></Project>";
    ASSERT_TRUE(fs.writeFileAtomic(userFile, user).has_value());
    const auto before = processes();
    OwnedProcesses owned;
    WorkDir work("r2a-debug");
    WorkbenchRig rig(work);
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
    ASSERT_TRUE(debug->breakpoints().at(source, 641));
    EXPECT_EQ(debug->breakpoints().at(source, 641)->status, frost::debug::BreakpointStatus::Verified);
    EXPECT_EQ(debug->selectedFrame()->line, 641);
    EXPECT_NE(debug->selectedFrame()->name.find("wWinMain"), std::string::npos);
    std::cout << "R2A verified stop at wWinMain line=" << debug->selectedFrame()->line << std::endl;
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
        std::cout << "R2A scope=" << scopeName.toStdString() << " pending=" << variables->pendingRequests()
                  << std::endl;
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
    std::cout << "R2A Locals contained pCmdLine with the requested arguments" << std::endl;
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
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child.pid);
    ASSERT_NE(process, nullptr);
    owned.handles.push_back(process);
    const auto running = processes();
    const auto parent = std::ranges::find_if(running, [&](const auto& item) { return item.pid == child.parent; });
    ASSERT_NE(parent, running.end());
    ASSERT_TRUE(QString::fromStdWString(parent->path).endsWith(QStringLiteral("lldb-dap.exe"), Qt::CaseInsensitive));
    ASSERT_TRUE(std::ranges::none_of(before, [&](const auto& old) { return old.pid == parent->pid; }));
    HANDLE adapter =
        OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent->pid);
    ASSERT_NE(adapter, nullptr);
    owned.handles.push_back(adapter);
    std::cout << "R2A debuggee PID=" << child.pid << " lldb-dap PID=" << parent->pid << std::endl;
    int previous = stops;
    ASSERT_TRUE(debug->stepInto());
    ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    EXPECT_NE(debug->selectedFrame()->name.find("parseCommandLine"), std::string::npos);
    std::cout << "R2A Step Into entered parseCommandLine" << std::endl;
    previous = stops;
    ASSERT_TRUE(debug->stepOut());
    ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    EXPECT_NE(debug->selectedFrame()->name.find("wWinMain"), std::string::npos);
    std::cout << "R2A Step Out returned to wWinMain" << std::endl;
    for (int count = 0; debug->selectedFrame()->line < 657 && count < 30; ++count) {
        previous = stops;
        ASSERT_TRUE(debug->stepOver());
        ASSERT_TRUE(waitFor([&] { return stops > previous && debug->selectedFrame().has_value(); }, 60s));
    }
    ASSERT_EQ(debug->selectedFrame()->line, 657);
    std::cout << "R2A Step Over reached line 657, first executable line after doPrintAndQuit" << std::endl;
    auto* watches = window->watchesModel();
    watches->addWatch(QStringLiteral("isMultiInst"));
    ASSERT_TRUE(waitFor([&] { return watches->pendingRequests() == 0; }, 60s));
    EXPECT_EQ(watches->valueAt(watches->index(0, 1)), QStringLiteral("true"));
    frost::ui::EvaluateDialog evaluate(*debug, QStringLiteral("doPrintAndQuit || doFunctionListExport"));
    evaluate.evaluate();
    ASSERT_TRUE(
        waitFor([&] { return evaluate.result()->rowCount() == 1 && evaluate.result()->pendingRequests() == 0; }, 60s));
    const QString result = evaluate.result()->valueAt(evaluate.result()->index(0, 1));
    std::cout << "R2A watch=" << watches->valueAt(watches->index(0, 1)).toStdString()
              << " evaluate=" << result.toStdString() << std::endl;
    EXPECT_TRUE(result.endsWith(QStringLiteral("false")));
    ASSERT_TRUE(debug->resume());
    ASSERT_TRUE(waitFor([&] { return visibleWindow(child.pid); }, 120s));
    std::cout << "R2A Notepad++ window visible on private desktop" << std::endl;
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
    std::cout << "R2A pause showed " << pausedThreadCount << " threads; child and adapter stopped" << std::endl;
}
} // namespace
