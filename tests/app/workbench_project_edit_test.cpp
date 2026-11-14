// SPDX-License-Identifier: Apache-2.0
// Real Windows ProjectHost removal through Workbench on isolated solution fixtures.
#include "frost/build/msbuild_locator.hpp"
#include "frost/document/file_document.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/text/document.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QScopeGuard>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>

#include <windows.h>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

std::string launchExecutables(frost::io::ScriptedProcessLauncher& launcher) {
    std::ostringstream out;
    for (const auto& launch : launcher.launches())
        out << "\n  " << launch.spec.executable;
    return out.str();
}

class ReloadFaultFs : public frost::io::IFileSystem {
public:
    virtual ~ReloadFaultFs() = default;
    explicit ReloadFaultFs(std::string solution)
        : m_solution(std::move(solution)), m_real(frost::io::realFileSystem()) {}
    frost::Result<std::string> readFile(std::string_view path) override {
        if (path == m_solution && m_fail.exchange(false))
            return frost::fail(frost::ErrorCode::IoError, "injected post-commit reload failure");
        return m_real.readFile(path);
    }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_real.writeFileAtomic(path, data);
    }
    frost::Result<frost::io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                                        const frost::io::SafeWriteOptions& options) override {
        return m_real.safeWrite(path, data, options);
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override { return m_real.status(path); }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        return m_real.listDirectory(path);
    }
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        return m_real.listDirectoryWithDiagnostics(path);
    }
    frost::Result<void> createDirectories(std::string_view path) override { return m_real.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override {
        auto result = m_real.removeFile(path);
        if (m_arm.load() && path.ends_with(".json") && path.find("project-edits") != std::string_view::npos)
            m_fail.store(true);
        return result;
    }
    frost::Result<void> removeDirectory(std::string_view path) override { return m_real.removeDirectory(path); }
    frost::Result<bool> exists(std::string_view path) override { return m_real.exists(path); }
    frost::Result<std::string> canonicalPath(std::string_view path) override { return m_real.canonicalPath(path); }
    frost::Result<std::string> physicalPath(std::string_view path) override { return m_real.physicalPath(path); }
    void arm() { m_arm.store(true); }

private:
    std::string m_solution;
    frost::io::IFileSystem& m_real;
    std::atomic<bool> m_arm{false}, m_fail{false};
};

class HeldSnapshotFs final : public ReloadFaultFs {
public:
    HeldSnapshotFs(std::string solution, std::string assembly)
        : ReloadFaultFs(std::move(solution)), m_assembly(std::move(assembly)) {}
    frost::Result<std::string> readFile(std::string_view path) override {
        if (path == m_assembly) {
            std::unique_lock lock(m_mutex);
            m_entered.store(true);
            m_release.wait(lock, [this] { return m_released; });
        }
        return ReloadFaultFs::readFile(path);
    }
    bool entered() const { return m_entered.load(); }
    void release() {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_release.notify_all();
    }

private:
    std::string m_assembly;
    std::mutex m_mutex;
    std::condition_variable m_release;
    std::atomic<bool> m_entered{false};
    bool m_released = false;
};

// A controlled owned evaluator holds a real Windows sharing constraint until cancellation AND
// the test's completion latch. This distinguishes requesting cancellation from joining readers.
struct HeldProjectReader {
    ~HeldProjectReader() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool released = false;
    std::atomic<bool> entered = false, cancelled = false, returned = false;
};

class HeldProjectReaderProcess final : public frost::io::IProcess {
public:
    HeldProjectReaderProcess(std::unique_ptr<IProcess> process, std::shared_ptr<HeldProjectReader> reader)
        : m_process(std::move(process)), m_reader(std::move(reader)) {}
    frost::io::ProcessId pid() const noexcept override { return m_process->pid(); }
    frost::Result<void> writeStdin(std::string_view bytes) override { return m_process->writeStdin(bytes); }
    void closeStdin() noexcept override { m_process->closeStdin(); }
    frost::Result<std::size_t> readStdout(std::span<char>) override {
        std::unique_lock lock(m_reader->mutex);
        m_reader->entered.store(true);
        const bool complete =
            m_reader->changed.wait_for(lock, 30s, [&] { return m_reader->cancelled.load() && m_reader->released; });
        if (m_reader->handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_reader->handle);
        m_reader->handle = INVALID_HANDLE_VALUE;
        m_reader->returned.store(true);
        if (!complete)
            return frost::fail(frost::ErrorCode::Timeout, "held project reader was not released after cancellation");
        return std::size_t{0};
    }
    frost::Result<std::size_t> readStderr(std::span<char> bytes) override { return m_process->readStderr(bytes); }
    frost::Result<frost::io::ExitStatus> wait(std::optional<frost::IClock::Duration> timeout) override {
        return m_process->wait(timeout);
    }
    frost::Result<void> terminate() override {
        m_reader->cancelled.store(true);
        m_reader->changed.notify_all();
        return m_process->terminate();
    }
    frost::Result<frost::io::ProcessResult> communicate(std::string_view input,
                                                        frost::IClock::Duration timeout) override {
        return m_process->communicate(input, timeout);
    }
    void abandonOutput() noexcept override { m_process->abandonOutput(); }

private:
    std::unique_ptr<IProcess> m_process;
    std::shared_ptr<HeldProjectReader> m_reader;
};

// Only the first evaluation is controlled. ProjectHost and the subsequent evaluation use the
// actual installed tools, keeping the edit's bytes and refreshed model as independent oracles.
class ProjectReaderLauncher final : public frost::io::IProcessLauncher {
public:
    ProjectReaderLauncher(std::string msbuild, std::string project)
        : m_msbuild(std::move(msbuild)), m_project(std::move(project)) {
        m_script.expect({.neverExits = true});
    }
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        if (spec.executable == m_msbuild && evaluations.fetch_add(1) == 0) {
            auto path = frost::io::toWin32Path(m_project);
            if (!path)
                return std::unexpected(path.error());
            reader->handle =
                CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (reader->handle == INVALID_HANDLE_VALUE)
                return frost::fail(frost::ErrorCode::IoError, "cannot hold the real project reader");
            auto scripted = m_script.launch(spec);
            if (!scripted)
                return std::unexpected(scripted.error());
            return std::make_unique<HeldProjectReaderProcess>(std::move(*scripted), reader);
        }
        if (spec.executable == FROST_PROJECT_HOST) {
            mutationBeforeReaderReturned.store(!reader->returned.load());
            mutationStarted.store(true);
        }
        if (spec.executable == m_msbuild) {
            const auto bytes = frost::io::realFileSystem().readFile(m_project);
            if (mutationStarted.load()) {
                resumedWithRemovedSource.store(!bytes || bytes->find("extra.cpp") != std::string::npos);
                resumedAfterMutation.store(true);
            }
        }
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    std::shared_ptr<HeldProjectReader> reader = std::make_shared<HeldProjectReader>();
    std::atomic<unsigned> evaluations = 0;
    std::atomic<bool> mutationStarted = false, mutationBeforeReaderReturned = false;
    std::atomic<bool> resumedWithRemovedSource = false;
    std::atomic<bool> resumedAfterMutation = false;

private:
    std::string m_msbuild, m_project;
    frost::io::ScriptedProcessLauncher m_script;
};

QJsonObject journalEntry(const std::string& path, const std::string& before, const std::string& after) {
    auto wide = frost::io::toWin32Path(path);
    if (!wide) {
        ADD_FAILURE() << wide.error().describe();
        return {};
    }
    HANDLE handle = CreateFileW(wide->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ADD_FAILURE() << "CreateFileW failed";
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != FALSE;
    CloseHandle(handle);
    if (!ok) {
        ADD_FAILURE() << "GetFileInformationByHandle failed";
        return {};
    }
    const auto encoded = [](const std::string& bytes) {
        return QString::fromLatin1(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toBase64());
    };
    return {{"Path", QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()))},
            {"Before", encoded(before)},
            {"After", encoded(after)},
            {"Volume", static_cast<qint64>(info.dwVolumeSerialNumber)},
            {"IndexHigh", static_cast<qint64>(info.nFileIndexHigh)},
            {"IndexLow", static_cast<qint64>(info.nFileIndexLow)}};
}

void writeInPlace(const std::string& path, const std::string& bytes) {
    auto wide = frost::io::toWin32Path(path);
    ASSERT_TRUE(wide);
    HANDLE handle = CreateFileW(wide->c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                    written == static_cast<DWORD>(bytes.size()) && SetEndOfFile(handle) != FALSE &&
                    FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    ASSERT_TRUE(ok);
}

TEST(WorkbenchProjectEdit, AddsExistingSourceThroughProjectMenu) {
    auto& fs = frost::io::realFileSystem();
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), fs);
    if (!found)
        GTEST_SKIP() << found.error().describe();
    WorkDir dir("add source", "tiny_sln");
    WorkDir privateData("add source state");
    const auto source = dir.join("App/nested å 😀/extra source.cpp");
    const auto header = dir.join("App/nested å 😀/first header.hpp");
    dir.write("App/nested å 😀/extra source.cpp", "int extra() { return 1; }\n");
    dir.write("App/nested å 😀/first header.hpp", "#pragma once\n");
    dir.write("App/App.vcxproj.filters", "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">"
                                         "<!-- preserve --><ItemGroup><ClCompile Include=\"main.cpp\" />"
                                         "</ItemGroup></Project>");
    const auto beforeProject = dir.read("App/App.vcxproj");
    const auto beforeFilters = dir.read("App/App.vcxproj.filters");
    RigOptions options;
    options.workbench.msbuildPath = found->msbuildPath;
    options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    std::string selected = source;
    // QFileDialog returns forward-slash paths on Windows, even though WorkDir uses native separators.
    options.workbench.selectExistingProjectSource = [&](const QString&) {
        return QString::fromStdString(selected).replace('\\', '/');
    };
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto* explorer = rig.workbench->solutionExplorer();
    auto projectId = rig.workbench->solution()->findProjectByPath(dir.join("App/App.vcxproj"));
    ASSERT_TRUE(projectId);
    const auto modelProject = rig.workbench->solution()->project(*projectId).path;
    EXPECT_EQ(fs.canonicalPath(source).value_or(""), source);
    EXPECT_EQ(fs.canonicalPath(modelProject).value_or(""), modelProject);
    EXPECT_TRUE(rig.trust->isTrusted(source));
    EXPECT_EQ(QString::fromStdString(source).toUtf8().toStdString(), source);
    EXPECT_TRUE(source.starts_with(frost::io::parentPath(modelProject)));
    ASSERT_TRUE(fs.status(source));
    EXPECT_EQ(fs.status(source)->type, frost::io::FileType::Regular);
    const auto index = explorer->filterModel()->mapFromSource(
        explorer->model()->indexOf(rig.workbench->solution()->project(*projectId).node));
    ASSERT_TRUE(index.isValid());
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
    const auto actions = menu->actions();
    const auto add = std::ranges::find_if(
        actions, [](QAction* action) { return action->objectName() == QStringLiteral("addExistingProjectFile"); });
    ASSERT_NE(add, actions.end());
    const auto notifications = rig.window->notifications().log().size();
    (*add)->trigger();
    ASSERT_TRUE(waitFor(
        [&] {
            return explorer->model()->indexOfPath(source).isValid() ||
                   rig.window->notifications().log().size() > notifications;
        },
        120s));
    if (!explorer->model()->indexOfPath(source).isValid()) {
        const auto& last = rig.window->notifications().log().back().notification;
        FAIL() << last.title.toStdString() << ": " << last.text.toStdString();
    }
    /*
     * SLN-6 (ADR 0082): MSBuild's construction API places the item among the group's items of its
     * type in order (after main.cpp) and indents it as the document does; the one-line filters
     * file gets it inline.
     */
    const std::string insertion = "<ClCompile Include=\"nested å 😀\\extra source.cpp\" />";
    const std::string mainItem = "<ClCompile Include=\"main.cpp\" />";
    auto expectedProject = beforeProject;
    expectedProject.insert(expectedProject.find(mainItem) + mainItem.size(), "\n    " + insertion);
    auto expectedFilters = beforeFilters;
    expectedFilters.insert(expectedFilters.find(mainItem) + mainItem.size(), insertion);
    EXPECT_EQ(dir.read("App/App.vcxproj"), expectedProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), expectedFilters);
    EXPECT_EQ(dir.read("App/nested å 😀/extra source.cpp"), "int extra() { return 1; }\n");
    selected = header;
    projectId = rig.workbench->solution()->findProjectByPath(dir.join("App/App.vcxproj"));
    ASSERT_TRUE(projectId);
    const auto projectIndex = explorer->filterModel()->mapFromSource(
        explorer->model()->indexOf(rig.workbench->solution()->project(*projectId).node));
    ASSERT_TRUE(projectIndex.isValid());
    std::unique_ptr<QMenu> headerMenu(explorer->createContextMenu(projectIndex));
    const auto headerActions = headerMenu->actions();
    const auto addHeader = std::ranges::find_if(headerActions, [](QAction* action) {
        return action->objectName() == QStringLiteral("addExistingProjectFile");
    });
    ASSERT_NE(addHeader, headerActions.end());
    const auto beforeHeaderNotifications = rig.window->notifications().log().size();
    (*addHeader)->trigger();
    ASSERT_TRUE(waitFor(
        [&] {
            return explorer->model()->indexOfPath(header).isValid() ||
                   rig.window->notifications().log().size() > beforeHeaderNotifications;
        },
        120s));
    if (!explorer->model()->indexOfPath(header).isValid()) {
        const auto& last = rig.window->notifications().log().back().notification;
        FAIL() << last.title.toStdString() << ": " << last.text.toStdString();
    }
    // A ClInclude has no group of its type yet: a new group follows the last item group.
    const std::string headerItem = "<ClInclude Include=\"nested å 😀\\first header.hpp\" />";
    expectedProject.insert(expectedProject.find("  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\""),
                           "  <ItemGroup>\n    " + headerItem + "\n  </ItemGroup>\n");
    expectedFilters.insert(expectedFilters.rfind("</Project>"), "<ItemGroup>\n  " + headerItem + "\n</ItemGroup>");
    EXPECT_EQ(dir.read("App/App.vcxproj"), expectedProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), expectedFilters);
    EXPECT_EQ(dir.read("App/nested å 😀/first header.hpp"), "#pragma once\n");
    dir.write("AppSibling/evil.cpp", "int untouched;\n");
    selected = dir.join("AppSibling/evil.cpp");
    projectId = rig.workbench->solution()->findProjectByPath(dir.join("App/App.vcxproj"));
    ASSERT_TRUE(projectId);
    const auto siblingProjectIndex = explorer->filterModel()->mapFromSource(
        explorer->model()->indexOf(rig.workbench->solution()->project(*projectId).node));
    ASSERT_TRUE(siblingProjectIndex.isValid());
    std::unique_ptr<QMenu> siblingMenu(explorer->createContextMenu(siblingProjectIndex));
    const auto siblingActions = siblingMenu->actions();
    const auto siblingAdd = std::ranges::find_if(siblingActions, [](QAction* action) {
        return action->objectName() == QStringLiteral("addExistingProjectFile");
    });
    ASSERT_NE(siblingAdd, siblingActions.end());
    const auto beforeSiblingNotifications = rig.window->notifications().log().size();
    (*siblingAdd)->trigger();
    ASSERT_GT(rig.window->notifications().log().size(), beforeSiblingNotifications);
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("inside the trusted project"));
    EXPECT_EQ(dir.read("App/App.vcxproj"), expectedProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), expectedFilters);
    EXPECT_EQ(dir.read("AppSibling/evil.cpp"), "int untouched;\n");
    selected = dir.join("App/nested å 😀") + "\\..\\nested å 😀\\extra source.cpp";
    const auto beforeAliasNotifications = rig.window->notifications().log().size();
    (*siblingAdd)->trigger();
    ASSERT_GT(rig.window->notifications().log().size(), beforeAliasNotifications);
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("without aliases"));
    EXPECT_EQ(dir.read("App/App.vcxproj"), expectedProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), expectedFilters);
    frost::io::ProcessSpec spec;
    spec.executable = found->msbuildPath;
    spec.arguments = {dir.join("App/App.vcxproj"), "/t:Build", "/p:Configuration=Debug", "/p:Platform=x64", "/m:1"};
    spec.workingDirectory = dir.path();
    const auto built = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 120s);
    ASSERT_TRUE(built);
    EXPECT_EQ(built->exitCode, 0);
}

TEST(WorkbenchProjectEdit, ManagedPreparationBlocksRemovalUntilWorkspaceReopened) {
    WorkDir dir("managed preparation edit", "tiny_sln");
    WorkDir privateData("managed preparation edit state");
    const auto sourcePath = dir.join("App/extra.cpp");
    auto project = dir.read("App/App.vcxproj");
    const auto position = project.find("<ClCompile Include=\"main.cpp\" />");
    ASSERT_NE(position, std::string::npos);
    project.insert(position, "<ClCompile Include=\"extra.cpp\" />");
    dir.write("App/App.vcxproj", project);
    dir.write("App/App.vcxproj.filters",
              "<Project><ItemGroup><ClCompile Include=\"extra.cpp\" /></ItemGroup></Project>");
    const auto filters = dir.read("App/App.vcxproj.filters");
    dir.write("App/extra.cpp", "int extra;\n");
    dir.write("Managed.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    dir.write("dotnet.exe", "fixture");
    dir.write("guard.dll", "fixture");
    dir.write("adapter.exe", "fixture");
    dir.write("host.exe", "fixture");
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dir.join("dotnet.exe"), .neverExits = true});
    launcher.expect({.expectExecutable = dir.join("host.exe"), .neverExits = true});
    RigOptions options;
    options.disableProjectEvaluation = true;
    options.launcher = &launcher;
    options.workbench.msbuildPath = dir.join("msbuild.exe");
    options.workbench.projectEditHostPath = dir.join("host.exe");
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    options.workbench.managedPdbGuardPath = dir.join("guard.dll");
    options.workbench.selectManagedDebug = [&](std::string_view) -> std::optional<frost::app::ManagedDebugSelection> {
        return frost::app::ManagedDebugSelection{.project = dir.join("Managed.csproj"),
                                                 .dotnet = dir.join("dotnet.exe"),
                                                 .configuration = "Debug",
                                                 .framework = "net10.0",
                                                 .runtimeIdentifier = "",
                                                 .adapter = dir.join("adapter.exe")};
    };
    WorkbenchRig rig(dir, std::move(options));
    // Keep ambient language servers out of the scripted debug and project-host launches.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto debug = rig.workbench->registry().find("run.debugManaged");
    ASSERT_TRUE(debug);
    frost::input::ActionContext context;
    context.trusted = true;
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*debug, context), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 1; }, 10s)) << launchExecutables(launcher);
    auto triggerRemove = [&] {
        auto* explorer = rig.workbench->solutionExplorer();
        const auto index = explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(sourcePath));
        EXPECT_TRUE(index.isValid());
        std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
        const auto actions = menu->actions();
        const auto remove = std::ranges::find_if(
            actions, [](QAction* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
        EXPECT_NE(remove, actions.end());
        if (remove != actions.end())
            (*remove)->trigger();
    };
    const auto notifications = rig.window->notifications().log().size();
    triggerRemove();
    ASSERT_GT(rig.window->notifications().log().size(), notifications);
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("wait for any build"));
    EXPECT_EQ(launcher.launches().size(), 1U);
    EXPECT_FALSE(frost::io::realFileSystem().exists(privateData.join("project-edits")).value_or(true));
    EXPECT_EQ(dir.read("App/App.vcxproj"), project);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), filters);
    EXPECT_EQ(dir.read("App/extra.cpp"), "int extra;\n");
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.slnx")));
    ASSERT_TRUE(waitFor([&] { return launcher.launches().front().terminated; }, 10s));
    ASSERT_TRUE(waitFor([&] { return rig.workbench->registry().isEnabled(*debug, context); }, 10s));
    triggerRemove();
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 2; }, 10s));
    EXPECT_EQ(launcher.launches().back().spec.executable, dir.join("host.exe"));
}

TEST(WorkbenchProjectEdit, PendingManagedSnapshotBlocksRemovalUntilCancelled) {
    WorkDir dir("managed snapshot edit", "tiny_sln");
    WorkDir privateData("managed snapshot edit state");
    const auto sourcePath = dir.join("App/extra.cpp");
    auto project = dir.read("App/App.vcxproj");
    const auto position = project.find("<ClCompile Include=\"main.cpp\" />");
    ASSERT_NE(position, std::string::npos);
    project.insert(position, "<ClCompile Include=\"extra.cpp\" />");
    dir.write("App/App.vcxproj", project);
    dir.write("App/App.vcxproj.filters",
              "<Project><ItemGroup><ClCompile Include=\"extra.cpp\" /></ItemGroup></Project>");
    const auto filters = dir.read("App/App.vcxproj.filters");
    dir.write("App/extra.cpp", "int extra;\n");
    dir.write("Managed.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\" />");
    dir.write("Program.cs", "source");
    dir.write("Managed.dll", "assembly");
    dir.write("Managed.pdb", "symbols");
    dir.write("dotnet.exe", "fixture");
    dir.write("guard.dll", "fixture");
    dir.write("adapter.exe", "fixture");
    dir.write("host.exe", "fixture");
    const auto hash = [](const char* bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(QByteArray(bytes), QCryptographicHash::Sha256).toHex());
    };
    const QJsonObject properties{{"TargetPath", QString::fromStdString(dir.join("Managed.dll"))},
                                 {"TargetFramework", "net10.0"},
                                 {"TargetFrameworks", ""},
                                 {"OutputType", "Exe"},
                                 {"RuntimeIdentifier", ""}};
    const QJsonObject manifest{
        {"moduleId", "adc9a846-ff68-4338-95ab-732080f453c5"},
        {"assemblySha256", hash("assembly")},
        {"pdbSha256", hash("symbols")},
        {"sources", QJsonArray{QJsonObject{{"path", QString::fromStdString(dir.join("Program.cs"))},
                                           {"sha256", hash("source")}}}}};
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = dir.join("dotnet.exe")});
    launcher.expect(
        {.expectExecutable = dir.join("dotnet.exe"),
         .stdoutBytes =
             QJsonDocument(QJsonObject{{"Properties", properties}}).toJson(QJsonDocument::Compact).toStdString()});
    launcher.expect({.expectExecutable = dir.join("dotnet.exe"),
                     .stdoutBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact).toStdString()});
    launcher.expect({.expectExecutable = dir.join("host.exe"), .neverExits = true});
    HeldSnapshotFs fs(dir.join("Tiny.sln"), dir.join("Managed.dll"));
    RigOptions options;
    options.disableProjectEvaluation = true;
    options.fileSystem = &fs;
    options.launcher = &launcher;
    options.workbench.msbuildPath = dir.join("msbuild.exe");
    options.workbench.projectEditHostPath = dir.join("host.exe");
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    options.workbench.managedPdbGuardPath = dir.join("guard.dll");
    options.workbench.selectManagedDebug = [&](std::string_view) -> std::optional<frost::app::ManagedDebugSelection> {
        return frost::app::ManagedDebugSelection{.project = dir.join("Managed.csproj"),
                                                 .dotnet = dir.join("dotnet.exe"),
                                                 .configuration = "Debug",
                                                 .framework = "net10.0",
                                                 .runtimeIdentifier = "",
                                                 .adapter = dir.join("adapter.exe")};
    };
    WorkbenchRig rig(dir, std::move(options));
    const auto release = qScopeGuard([&] { fs.release(); });
    // Keep ambient language servers out of the scripted debug and project-host launches.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsManagedServerPath,
                                  dir.join("missing-language-server.dll")));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::toolsLuaLanguageServerPath,
                                  dir.join("missing-lua-language-server.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto debug = rig.workbench->registry().find("run.debugManaged");
    ASSERT_TRUE(debug);
    frost::input::ActionContext context;
    context.trusted = true;
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*debug, context), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return fs.entered(); }, 10s));
    ASSERT_EQ(launcher.launches().size(), 3U) << launchExecutables(launcher);
    auto triggerRemove = [&] {
        auto* explorer = rig.workbench->solutionExplorer();
        const auto index = explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(sourcePath));
        EXPECT_TRUE(index.isValid());
        std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
        const auto actions = menu->actions();
        const auto remove = std::ranges::find_if(
            actions, [](QAction* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
        EXPECT_NE(remove, actions.end());
        if (remove != actions.end())
            (*remove)->trigger();
    };
    const auto notifications = rig.window->notifications().log().size();
    triggerRemove();
    ASSERT_GT(rig.window->notifications().log().size(), notifications);
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("wait for any build"));
    EXPECT_EQ(launcher.launches().size(), 3U);
    EXPECT_FALSE(frost::io::realFileSystem().exists(privateData.join("project-edits")).value_or(true));
    EXPECT_EQ(dir.read("App/App.vcxproj"), project);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), filters);
    EXPECT_EQ(dir.read("App/extra.cpp"), "int extra;\n");
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.slnx")));
    fs.release();
    triggerRemove();
    ASSERT_TRUE(waitFor([&] { return launcher.launches().size() == 4; }, 10s));
    EXPECT_EQ(launcher.launches().back().spec.executable, dir.join("host.exe"));
}

TEST(WorkbenchProjectEdit, OwnedReadersJoinBeforeExclusiveEditsAndDeferredTrustRefreshHonorsCancellation) {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    ASSERT_TRUE(found) << found.error().describe();
    enum class Outcome { Commit, RevokeTrust, Shutdown };
    for (const auto outcome : {Outcome::Commit, Outcome::RevokeTrust, Outcome::Shutdown}) {
        SCOPED_TRACE(static_cast<int>(outcome));
        WorkDir dir("held project reader", "tiny_sln");
        WorkDir privateData("reader edit state");
        const auto projectPath = dir.join("App/App.vcxproj");
        const auto sourcePath = dir.join("App/extra.cpp");
        const std::string diskSource = "int extra() { return 1; }\n";
        dir.write("App/extra.cpp", diskSource);
        auto project = dir.read("App/App.vcxproj");
        const std::string mainItem = "<ClCompile Include=\"main.cpp\" />";
        const auto position = project.find(mainItem);
        ASSERT_NE(position, std::string::npos);
        project.insert(position + mainItem.size(), "\n    <ClCompile Include=\"extra.cpp\" />");
        dir.write("App/App.vcxproj", project);
        ProjectReaderLauncher launcher(found->msbuildPath, projectPath);
        RigOptions options;
        options.launcher = &launcher;
        options.workbench.workers = 1;
        options.workbench.msbuildPath = found->msbuildPath;
        options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
        options.workbench.projectEditStateDirectory = privateData.join("project-edits");
        auto rig = std::make_unique<WorkbenchRig>(dir, std::move(options));
        // Every fatal assertion releases the reader before Workbench joins its owned workers.
        const auto release = qScopeGuard([&] { launcher.reader->release(); });
        ASSERT_TRUE(rig->trust->trust(dir.path()));
        ASSERT_TRUE(rig->workbench->openPath(dir.join("Tiny.sln")));
        ASSERT_TRUE(rig->workbench->openPath(sourcePath));
        auto* document = rig->workbench->documentRegistry().find(sourcePath);
        ASSERT_NE(document, nullptr);
        ASSERT_TRUE(document->document().replace(frost::text::ByteRange{frost::ByteOffset{0}, frost::ByteOffset{0}},
                                                 "// unsaved\n", frost::text::EditContext{}));
        ASSERT_TRUE(waitFor([&] { return launcher.reader->entered.load(); }, 10s));
        auto wide = frost::io::toWin32Path(projectPath);
        ASSERT_TRUE(wide);
        HANDLE writer = CreateFileW(wide->c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        const auto writerError = GetLastError();
        if (writer != INVALID_HANDLE_VALUE)
            CloseHandle(writer);
        ASSERT_EQ(writer, INVALID_HANDLE_VALUE);
        ASSERT_EQ(writerError, ERROR_SHARING_VIOLATION);

        // Advance the observed trust revision before the request captures its admission. The
        // actual trust timer callback must defer this refresh while the edit owns the reservation.
        ASSERT_TRUE(rig->trust->trust(privateData.path()));
        auto* explorer = rig->workbench->solutionExplorer();
        const auto index = explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(sourcePath));
        ASSERT_TRUE(index.isValid());
        std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
        const auto actions = menu->actions();
        const auto remove = std::ranges::find_if(
            actions, [](QAction* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
        ASSERT_NE(remove, actions.end());
        (*remove)->trigger();
        ASSERT_TRUE(waitFor([&] { return launcher.reader->cancelled.load(); }, 10s));
        EXPECT_TRUE(rig->workbench->buildService().projectEditInProgress());
        auto* trustWatch = rig->workbench->findChild<QTimer*>(QStringLiteral("projectTrustWatch"));
        ASSERT_NE(trustWatch, nullptr);
        ASSERT_TRUE(QMetaObject::invokeMethod(trustWatch, "timeout", Qt::DirectConnection));
        EXPECT_EQ(launcher.evaluations.load(), 1U);
        EXPECT_FALSE(launcher.mutationStarted.load());
        EXPECT_FALSE(launcher.reader->returned.load());
        EXPECT_EQ(dir.read("App/App.vcxproj"), project);
        EXPECT_TRUE(document->dirty());
        bool guiHeartbeat = false;
        QTimer::singleShot(0, rig->workbench.get(), [&] { guiHeartbeat = true; });
        ASSERT_TRUE(waitFor([&] { return guiHeartbeat; }, 10s));

        if (outcome == Outcome::Shutdown) {
            rig->workbench->shutdown();
            launcher.reader->release();
            rig.reset(); // Joins all retirement/admission tasks, then discards their GUI callbacks.
            QApplication::processEvents();
            EXPECT_TRUE(launcher.reader->returned.load());
            EXPECT_FALSE(launcher.mutationStarted.load());
            EXPECT_EQ(launcher.evaluations.load(), 1U);
            EXPECT_EQ(dir.read("App/App.vcxproj"), project);
            EXPECT_EQ(dir.read("App/extra.cpp"), diskSource);
            continue;
        }
        if (outcome == Outcome::RevokeTrust)
            ASSERT_TRUE(rig->trust->untrust(dir.path()));
        launcher.reader->release();
        ASSERT_TRUE(waitFor([&] { return !rig->workbench->buildService().projectEditInProgress(); }, 120s));
        EXPECT_TRUE(launcher.reader->returned.load());
        EXPECT_FALSE(launcher.mutationBeforeReaderReturned.load());
        EXPECT_TRUE(document->dirty());
        EXPECT_EQ(dir.read("App/extra.cpp"), diskSource);
        if (outcome == Outcome::RevokeTrust) {
            EXPECT_FALSE(launcher.mutationStarted.load());
            EXPECT_EQ(launcher.evaluations.load(), 1U);
            EXPECT_EQ(dir.read("App/App.vcxproj"), project);
            ASSERT_TRUE(rig->trust->trust(dir.path()));
            ASSERT_TRUE(QMetaObject::invokeMethod(trustWatch, "timeout", Qt::DirectConnection));
            ASSERT_TRUE(waitFor([&] { return launcher.evaluations.load() > 1; }, 10s));
            (*remove)->trigger();
            ASSERT_TRUE(waitFor([&] { return !rig->workbench->buildService().projectEditInProgress(); }, 120s));
            EXPECT_TRUE(launcher.mutationStarted.load());
            EXPECT_FALSE(launcher.mutationBeforeReaderReturned.load());
            EXPECT_TRUE(document->dirty());
            EXPECT_EQ(dir.read("App/extra.cpp"), diskSource);
            EXPECT_EQ(dir.read("App/App.vcxproj").find("extra.cpp"), std::string::npos);
            ASSERT_TRUE(waitFor([&] { return launcher.resumedAfterMutation.load(); }, 10s));
            EXPECT_FALSE(launcher.resumedWithRemovedSource.load());
        } else {
            EXPECT_TRUE(launcher.mutationStarted.load());
            ASSERT_TRUE(waitFor([&] { return launcher.resumedAfterMutation.load(); }, 10s));
            EXPECT_FALSE(launcher.resumedWithRemovedSource.load());
            EXPECT_FALSE(explorer->model()->indexOfPath(sourcePath).isValid());
            EXPECT_EQ(dir.read("App/App.vcxproj").find("extra.cpp"), std::string::npos);
        }
    }
}

TEST(WorkbenchProjectEdit, RemovesSourceFromBothSolutionFormatsWithoutSavingDirtyTab) {
    auto& fs = frost::io::realFileSystem();
    auto& launcher = frost::io::realProcessLauncher();
    auto found = frost::build::findMsBuild(launcher, fs);
    if (!found)
        GTEST_SKIP() << found.error().describe();
    for (std::string_view solution : {"Tiny.sln", "Tiny.slnx"}) {
        SCOPED_TRACE(std::string(solution));
        WorkDir dir("edit å 😀", "tiny_sln");
        WorkDir privateData("edit state");
        const std::string source = dir.join("App/extra.cpp");
        dir.write("App/extra.cpp", "int extra() { return 1; }\n");
        std::string project = dir.read("App/App.vcxproj");
        const std::string item = "<ClCompile Include=\"main.cpp\" />";
        const auto position = project.find(item);
        ASSERT_NE(position, std::string::npos);
        project.insert(position + item.size(), "\n    <ClCompile Include=\"extra.cpp\" />");
        dir.write("App/App.vcxproj", project);
        dir.write("App/App.vcxproj.filters",
                  "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">"
                  "<ItemGroup><ClCompile Include=\"extra.cpp\"><Filter>Source Files</Filter></ClCompile>"
                  "</ItemGroup></Project>");
        RigOptions options;
        options.workbench.msbuildPath = found->msbuildPath;
        options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
        options.workbench.projectEditStateDirectory = privateData.join("project-edits");
        WorkbenchRig rig(dir, std::move(options));
        ASSERT_TRUE(rig.workbench->openPath(dir.join(solution)));
        ASSERT_TRUE(rig.workbench->openPath(source));
        auto* document = rig.workbench->documentRegistry().find(source);
        ASSERT_NE(document, nullptr);
        ASSERT_TRUE(document->document().replace(frost::text::ByteRange{frost::ByteOffset{0}, frost::ByteOffset{0}},
                                                 "// unsaved\n", frost::text::EditContext{}));
        ASSERT_TRUE(document->dirty());
        auto* explorer = rig.workbench->solutionExplorer();
        ASSERT_NE(explorer, nullptr);
        const auto index = explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(source));
        ASSERT_TRUE(index.isValid());
        std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
        const auto actions = menu->actions();
        const auto action = std::ranges::find_if(actions, [](QAction* itemAction) {
            return itemAction->objectName() == QStringLiteral("removeFromProject");
        });
        ASSERT_NE(action, actions.end());
        (*action)->trigger();
        EXPECT_NE(dir.read("App/App.vcxproj").find("extra.cpp"), std::string::npos);
        ASSERT_TRUE(rig.trust->trust(dir.path()));
        const auto notificationsBefore = rig.window->notifications().log().size();
        (*action)->trigger();
        ASSERT_TRUE(waitFor(
            [&] {
                return !explorer->model()->indexOfPath(source).isValid() ||
                       rig.window->notifications().log().size() > notificationsBefore;
            },
            120s));
        if (explorer->model()->indexOfPath(source).isValid()) {
            const auto& last = rig.window->notifications().log().back().notification;
            FAIL() << last.title.toStdString() << ": " << last.text.toStdString();
        }
        const auto markers = fs.listDirectory(privateData.join("project-edits"));
        ASSERT_TRUE(markers);
        EXPECT_TRUE(markers->empty());
        EXPECT_TRUE(document->dirty());
        EXPECT_EQ(dir.read("App/extra.cpp"), "int extra() { return 1; }\n");
        EXPECT_EQ(rig.window->tabs().indexOf(source) >= 0, true);
        EXPECT_EQ(dir.read("App/App.vcxproj").find("extra.cpp"), std::string::npos);
        EXPECT_EQ(dir.read("App/App.vcxproj.filters").find("extra.cpp"), std::string::npos);
        frost::io::ProcessSpec spec;
        spec.executable = found->msbuildPath;
        spec.arguments = {dir.join(solution), "-m:1",    "-p:Configuration=Debug",
                          "-p:Platform=x64",  "-nologo", "-v:minimal"};
        spec.workingDirectory = dir.path();
        const auto built = frost::io::runToCompletion(launcher, spec, {}, 120s);
        ASSERT_TRUE(built) << built.error().describe();
        EXPECT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
    }
}

/*
 * SLN-6: a rename and a delete through the workbench update the .vcxproj and .vcxproj.filters
 * through the real ProjectHost: the rename changes only the Include attributes, the delete removes
 * the elements again (the project is byte-for-byte the fixture's), and MSBuild builds the result.
 */
TEST(WorkbenchProjectEdit, RenameAndDeleteUpdateTheProjectThroughProjectHost) {
    auto& fs = frost::io::realFileSystem();
    auto& launcher = frost::io::realProcessLauncher();
    auto found = frost::build::findMsBuild(launcher, fs);
    if (!found)
        GTEST_SKIP() << found.error().describe();
    WorkDir dir("rename edit", "tiny_sln");
    WorkDir privateData("rename edit state");
    const std::string fixtureProject = dir.read("App/App.vcxproj");
    dir.write("App/extra.cpp", "int extra() { return 1; }\n");
    std::string project = fixtureProject;
    const std::string item = "<ClCompile Include=\"main.cpp\" />";
    ASSERT_NE(project.find(item), std::string::npos);
    project.insert(project.find(item) + item.size(), "\n    <ClCompile Include=\"extra.cpp\" />");
    dir.write("App/App.vcxproj", project);
    const std::string filters = "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n"
                                "  <ItemGroup>\r\n    <ClCompile Include=\"extra.cpp\">\r\n"
                                "      <Filter>Source Files</Filter>\r\n    </ClCompile>\r\n  </ItemGroup>\r\n"
                                "  <ItemGroup>\r\n    <ClCompile Include=\"main.cpp\" />\r\n  </ItemGroup>\r\n"
                                "</Project>\r\n";
    dir.write("App/App.vcxproj.filters", filters);
    RigOptions options;
    options.workbench.msbuildPath = found->msbuildPath;
    options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    const auto failures = [&](std::size_t from) {
        std::string out;
        const auto& log = rig.window->notifications().log();
        for (std::size_t i = from; i < log.size(); ++i)
            if (log[i].notification.kind != frost::ui::NotificationKind::Info)
                out += log[i].notification.title.toStdString() + ": " + log[i].notification.text.toStdString() + "\n";
        return out;
    };

    // An edit has completed when ProjectHost removed its journal and the workbench its recovery record.
    const auto settled = [&] {
        const auto markers = fs.listDirectory(privateData.join("project-edits"));
        return !fs.exists(dir.join(".frost-project-edit-journal")).value_or(true) && markers && markers->empty();
    };

    // Rename extra.cpp to "renamed å.cpp": only the two Include attributes change.
    auto notices = rig.window->notifications().log().size();
    auto renamed = rig.workbench->renameItem(dir.join("App/extra.cpp"), "renamed å.cpp");
    ASSERT_TRUE(renamed) << renamed.error().describe();
    std::string expectedProject = project;
    expectedProject.replace(expectedProject.find("extra.cpp"), 9, "renamed å.cpp");
    std::string expectedFilters = filters;
    expectedFilters.replace(expectedFilters.find("extra.cpp"), 9, "renamed å.cpp");
    ASSERT_TRUE(waitFor(
        [&] { return (dir.read("App/App.vcxproj") != project && settled()) || !failures(notices).empty(); }, 120s));
    ASSERT_EQ(failures(notices), "");
    EXPECT_EQ(dir.read("App/App.vcxproj"), expectedProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), expectedFilters);
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->solution()->findFileNode(dir.join("App/renamed å.cpp")).has_value(); }, 60s));

    // Delete it: the elements go; the project is the fixture's again, byte for byte.
    notices = rig.window->notifications().log().size();
    ASSERT_TRUE(rig.workbench->deleteItem(dir.join("App/renamed å.cpp")));
    ASSERT_TRUE(waitFor(
        [&] { return (dir.read("App/App.vcxproj") != expectedProject && settled()) || !failures(notices).empty(); },
        120s));
    ASSERT_EQ(failures(notices), "");
    EXPECT_EQ(dir.read("App/App.vcxproj"), fixtureProject);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"),
              "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n"
              "  <ItemGroup>\r\n    <ClCompile Include=\"main.cpp\" />\r\n  </ItemGroup>\r\n</Project>\r\n");
    const auto markers = fs.listDirectory(privateData.join("project-edits"));
    ASSERT_TRUE(markers);
    EXPECT_TRUE(markers->empty());
    EXPECT_FALSE(fs.exists(dir.join(".frost-project-edit-journal")).value_or(true));
    frost::io::ProcessSpec spec;
    spec.executable = found->msbuildPath;
    spec.arguments = {dir.join("Tiny.sln"), "-m:1",    "-p:Configuration=Debug",
                      "-p:Platform=x64",    "-nologo", "-v:minimal",
                      "-nodeReuse:false"};
    spec.workingDirectory = dir.path();
    const auto built = frost::io::runToCompletion(launcher, spec, {}, 120s);
    ASSERT_TRUE(built) << built.error().describe();
    EXPECT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
}

TEST(WorkbenchProjectEdit, ExistingJournalBlocksBuildButDoesNotTrapProjectOrReplayIt) {
    WorkDir dir("edit journal", "tiny_sln");
    WorkDir privateData("unknown recovery state");
    dir.write(".frost-project-edit-journal", "unowned bytes");
    RigOptions options;
    options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    const auto blocked = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), frost::ErrorCode::Busy);
    EXPECT_EQ(dir.read(".frost-project-edit-journal"), "unowned bytes");
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    const auto projectId = rig.workbench->solution()->findProjectByPath(dir.join("App/App.vcxproj"));
    ASSERT_TRUE(projectId);
    const auto index = explorer->filterModel()->mapFromSource(
        explorer->model()->indexOf(rig.workbench->solution()->project(*projectId).node));
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
    const auto actions = menu->actions();
    const auto recover = std::ranges::find_if(
        actions, [](QAction* action) { return action->objectName() == QStringLiteral("recoverProjectEdit"); });
    ASSERT_NE(recover, actions.end());
    (*recover)->trigger();
    ASSERT_FALSE(rig.window->notifications().log().empty());
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("without a matching private"));
    EXPECT_EQ(rig.prompts.recoveryCalls, 0);
    EXPECT_EQ(dir.read(".frost-project-edit-journal"), "unowned bytes");
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.slnx")));
    EXPECT_EQ(dir.read(".frost-project-edit-journal"), "unowned bytes");
}

TEST(WorkbenchProjectEdit, FailedPostCommitReloadKeepsBuildReservationUntilReopen) {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!found)
        GTEST_SKIP() << found.error().describe();
    WorkDir dir("reload fault", "tiny_sln");
    WorkDir privateData("reload fault state");
    std::string project = dir.read("App/App.vcxproj");
    const std::string item = "<ClCompile Include=\"main.cpp\" />";
    const auto position = project.find(item);
    ASSERT_NE(position, std::string::npos);
    project.insert(position + item.size(), "\n    <ClCompile Include=\"extra.cpp\" />");
    dir.write("App/App.vcxproj", project);
    dir.write("App/App.vcxproj.filters",
              "<Project><ItemGroup><ClCompile Include=\"extra.cpp\" /></ItemGroup></Project>");
    dir.write("App/extra.cpp", "int extra;\n");
    ReloadFaultFs fault(dir.join("Tiny.sln"));
    RigOptions options;
    options.fileSystem = &fault;
    options.workbench.msbuildPath = found->msbuildPath;
    options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    auto* explorer = rig.workbench->solutionExplorer();
    const auto index =
        explorer->filterModel()->mapFromSource(explorer->model()->indexOfPath(dir.join("App/extra.cpp")));
    ASSERT_TRUE(index.isValid());
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
    const auto actions = menu->actions();
    const auto remove = std::ranges::find_if(
        actions, [](QAction* action) { return action->objectName() == QStringLiteral("removeFromProject"); });
    ASSERT_NE(remove, actions.end());
    fault.arm();
    const auto before = rig.window->notifications().log().size();
    (*remove)->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.window->notifications().log().size() > before; }, 120s));
    EXPECT_TRUE(rig.window->notifications().log().back().notification.text.contains("injected post-commit"));
    EXPECT_EQ(dir.read("App/App.vcxproj").find("extra.cpp"), std::string::npos);
    EXPECT_EQ(dir.read("App/extra.cpp"), "int extra;\n");
    EXPECT_TRUE(rig.workbench->buildService().projectEditInProgress());
    auto blocked = rig.workbench->build().build(frost::build::BuildAction::Build);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), frost::ErrorCode::Busy);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.slnx")));
    EXPECT_FALSE(rig.workbench->buildService().projectEditInProgress());
}

TEST(WorkbenchProjectEdit, OwnedJournalRecoversThroughProjectMenu) {
    auto& fs = frost::io::realFileSystem();
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), fs);
    if (!found)
        GTEST_SKIP() << found.error().describe();
    WorkDir dir("recovery å 😀", "tiny_sln");
    WorkDir privateData("recovery state");
    const std::string projectPath = dir.join("App/App.vcxproj");
    const std::string filterPath = projectPath + ".filters";
    std::string before = dir.read("App/App.vcxproj");
    const std::string sourceItem = "<ClCompile Include=\"extra.cpp\" />";
    const auto position = before.find("<ClCompile Include=\"main.cpp\" />");
    ASSERT_NE(position, std::string::npos);
    before.insert(position, sourceItem);
    const std::string after = [&] {
        auto result = before;
        result.erase(position, sourceItem.size());
        return result;
    }();
    const std::string filterBefore = "<Project><ItemGroup><ClCompile Include=\"extra.cpp\" /></ItemGroup></Project>";
    const std::string filterAfter = "<Project><ItemGroup></ItemGroup></Project>";
    dir.write("App/App.vcxproj", before);
    dir.write("App/App.vcxproj.filters", filterBefore);
    dir.write("App/extra.cpp", "int extra() { return 1; }\n");
    const QString id = QStringLiteral("9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15");
    const QString root = QString::fromStdString(dir.path());
    const QString projectName = QString::fromStdString(projectPath);
    QJsonObject record{{"Version", 2},
                       {"Root", root},
                       {"TransactionId", id},
                       {"Entries", QJsonArray{journalEntry(projectPath, before, after),
                                              journalEntry(filterPath, filterBefore, filterAfter)}}};
    const QByteArray journalBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const QString digest =
        QString::fromLatin1(QCryptographicHash::hash(journalBytes, QCryptographicHash::Sha256).toHex().toUpper());
    QJsonObject marker{{"Root", root}, {"Project", projectName}, {"TransactionId", id}, {"JournalDigest", digest}};
    privateData.write("project-edits/9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15.json",
                      QJsonDocument(marker).toJson(QJsonDocument::Compact).toStdString());
    dir.write(".frost-project-edit-journal", journalBytes.toStdString());
    writeInPlace(projectPath, after); // Known state after the first definition write; source remains intact.
    RigOptions options;
    options.workbench.msbuildPath = found->msbuildPath;
    options.workbench.projectEditHostPath = FROST_PROJECT_HOST;
    options.workbench.projectEditStateDirectory = privateData.join("project-edits");
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    ASSERT_TRUE(rig.workbench->buildService().projectEditInProgress());
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    const auto projectId = rig.workbench->solution()->findProjectByPath(projectPath);
    ASSERT_TRUE(projectId);
    const auto index = explorer->filterModel()->mapFromSource(
        explorer->model()->indexOf(rig.workbench->solution()->project(*projectId).node));
    ASSERT_TRUE(index.isValid());
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
    const auto actions = menu->actions();
    const auto action = std::ranges::find_if(
        actions, [](QAction* entry) { return entry->objectName() == QStringLiteral("recoverProjectEdit"); });
    ASSERT_NE(action, actions.end());
    (*action)->trigger(); // An untrusted root never reaches the recovery prompt.
    EXPECT_EQ(rig.prompts.recoveryCalls, 0);
    EXPECT_EQ(dir.read("App/App.vcxproj"), after);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    (*action)->trigger(); // A declined recovery leaves the journal and definitions untouched.
    EXPECT_EQ(rig.prompts.recoveryCalls, 1);
    EXPECT_EQ(dir.read("App/App.vcxproj"), after);
    EXPECT_TRUE(fs.exists(dir.join(".frost-project-edit-journal")).value_or(false));
    rig.prompts.recover = true;
    auto tampered = record;
    auto entries = tampered.value("Entries").toArray();
    auto firstEntry = entries[0].toObject();
    firstEntry.insert("Before", QString::fromLatin1(QByteArray("attacker definition").toBase64()));
    entries[0] = firstEntry;
    tampered.insert("Entries", entries);
    dir.write(".frost-project-edit-journal", QJsonDocument(tampered).toJson(QJsonDocument::Compact).toStdString());
    const auto beforeTamperNotice = rig.window->notifications().log().size();
    (*action)->trigger();
    ASSERT_TRUE(waitFor([&] { return rig.window->notifications().log().size() > beforeTamperNotice; }, 120s));
    EXPECT_EQ(dir.read("App/App.vcxproj"), after);
    EXPECT_TRUE(rig.workbench->buildService().projectEditInProgress());
    dir.write(".frost-project-edit-journal", journalBytes.toStdString());
    const auto notificationsBefore = rig.window->notifications().log().size();
    (*action)->trigger();
    ASSERT_TRUE(waitFor(
        [&] {
            return !rig.workbench->buildService().projectEditInProgress() ||
                   rig.window->notifications().log().size() > notificationsBefore;
        },
        120s));
    if (rig.workbench->buildService().projectEditInProgress()) {
        const auto& last = rig.window->notifications().log().back().notification;
        FAIL() << last.title.toStdString() << ": " << last.text.toStdString();
    }
    EXPECT_EQ(rig.prompts.recoveryCalls, 3);
    EXPECT_EQ(dir.read("App/App.vcxproj"), before);
    EXPECT_EQ(dir.read("App/App.vcxproj.filters"), filterBefore);
    EXPECT_EQ(dir.read("App/extra.cpp"), "int extra() { return 1; }\n");
    EXPECT_FALSE(fs.exists(dir.join(".frost-project-edit-journal")).value_or(true));
    EXPECT_FALSE(fs.exists(privateData.join("project-edits/9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15.json")).value_or(true));
}
} // namespace
