// SPDX-License-Identifier: Apache-2.0
//
// Test support for the workbench tests (workbench_test.cpp and the msbuild and clangd suites): a
// work directory under the build tree with a copy of a fixture, scripted prompts, and WorkbenchRig,
// a Workbench (the one composition root, with its navigation workbench) and its MainWindow over an
// application settings file, a trust store, sessions and a hot-exit journal in that directory, on
// the simulated en-US layout (offscreen; nothing reads the machine's keyboard layout or the user's
// settings).
#pragma once

#include "frost/app/main_window.hpp"
#include "frost/app/workbench.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/shell/editor_tabs.hpp"

#include "support/scratch_dir.hpp"
#include <QApplication>
#include <QDeadlineTimer>
#include <QKeyEvent>
#include <QWidget>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace frost::app::test {

// Processes events until `done` or `timeout` passes; true when `done` was reached.
inline bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    // Returns the evaluation that ended the wait: evaluating `done` a second time could see a state
    // that another thread (a CMake service, a language server) changed in between, and report a
    // condition that was reached as not reached.
    const QDeadlineTimer deadline(timeout);
    for (;;) {
        if (done())
            return true;
        if (deadline.hasExpired())
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// Sends a key press and release to `target` as the platform would.
inline void sendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(target, &release);
}

inline bool copyTree(io::IFileSystem& fs, const std::string& from, const std::string& to) {
    if (!fs.createDirectories(to)) {
        return false;
    }
    auto entries = fs.listDirectory(from);
    if (!entries) {
        return false;
    }
    for (const io::DirEntry& entry : *entries) {
        const std::string source = io::joinPath(from, entry.name).value_or("");
        const std::string target = io::joinPath(to, entry.name).value_or("");
        if (entry.type == io::FileType::Directory) {
            if (!copyTree(fs, source, target)) {
                return false;
            }
            continue;
        }
        auto bytes = fs.readFile(source);
        if (!bytes || !fs.writeFileAtomic(target, *bytes)) {
            return false;
        }
    }
    return true;
}

// A directory under FROST_APP_WORK_ROOT (the build tree, not %TEMP%, where MSBuild warns with
// MSB8029), removed afterwards; its name has a space. `fixture` ("" for none) names a directory
// under FROST_BUILD_FIXTURES copied into it.
class WorkDir {
public:
    explicit WorkDir(std::string_view tag, std::string_view fixture = {}) {
        static std::atomic<unsigned> counter{0};
        auto root = io::normalizePath(FROST_APP_WORK_ROOT);
        if (!root) {
            ADD_FAILURE() << "bad work root";
            return;
        }
        m_path = io::joinPath(*root, std::format("{} w-{}-{}", tag, QCoreApplication::applicationPid(), counter++))
                     .value_or("");
        frost::test::removeTree(io::realFileSystem(), m_path);
        EXPECT_TRUE(io::realFileSystem().createDirectories(m_path).has_value()) << m_path;
        if (!fixture.empty()) {
            auto source = io::normalizePath(std::format("{}/{}", FROST_BUILD_FIXTURES, fixture));
            EXPECT_TRUE(source && copyTree(io::realFileSystem(), *source, m_path)) << m_path;
        }
    }
    ~WorkDir() {
        if (!m_path.empty() && !frost::test::removeTree(io::realFileSystem(), m_path)) {
            ADD_FAILURE() << "could not remove " << m_path;
        }
    }
    WorkDir(const WorkDir&) = delete ("owns the directory");
    WorkDir& operator=(const WorkDir&) = delete ("owns the directory");

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    // path() joined with `relative` ('/' separators), normalised.
    [[nodiscard]] std::string join(std::string_view relative) const {
        return io::normalizePath(io::joinPath(m_path, relative).value_or("")).value_or("");
    }
    void write(std::string_view relative, std::string_view bytes) const {
        const std::string file = join(relative);
        ASSERT_TRUE(io::realFileSystem().createDirectories(std::string(io::parentPath(file))).has_value());
        ASSERT_TRUE(io::realFileSystem().writeFileAtomic(file, bytes).has_value()) << file;
    }
    [[nodiscard]] std::string read(std::string_view relative) const {
        return io::realFileSystem().readFile(join(relative)).value_or("<unreadable>");
    }

private:
    std::string m_path;
};

// Prompts with scripted answers; they count their calls.
class ScriptedPrompts final : public IWorkbenchPrompts {
public:
    [[nodiscard]] std::string chooseOpenPath(QWidget*, OpenKind kind) override {
        ++openCalls;
        lastOpenKind = kind;
        return openPath;
    }
    [[nodiscard]] std::string chooseSaveAsPath(QWidget*, const std::string& currentPath) override {
        ++saveAsPrompts;
        lastSaveAsCurrentPath = currentPath;
        return saveAsPath;
    }
    [[nodiscard]] std::string chooseNewItemName(QWidget*, const std::string& directory, bool folder) override {
        ++newItemPrompts;
        lastNewItemDirectory = directory;
        lastNewItemFolder = folder;
        return newItemName;
    }
    [[nodiscard]] std::optional<NewProjectSpec> chooseNewProject(QWidget*,
                                                                 const std::string& defaultLocation) override {
        ++newProjectPrompts;
        lastNewProjectDefaultLocation = defaultLocation;
        return newProject;
    }
    [[nodiscard]] bool confirmDiscard(QWidget*, const QString&, const QString& error) override {
        ++discardCalls;
        lastDiscardError = error;
        if (onDiscard)
            onDiscard();
        return discard;
    }
    [[nodiscard]] bool confirmRerun(QWidget*, const QString&) override {
        ++rerunCalls;
        return rerun;
    }
    [[nodiscard]] bool confirmProjectRecovery(QWidget*, const QString&) override {
        ++recoveryCalls;
        return recover;
    }
    [[nodiscard]] bool confirmExit(QWidget*) override {
        ++exitPrompts;
        return exitAnswer;
    }
    int exitPrompts = 0;    // general.confirmExit prompts.
    bool exitAnswer = true; // What they answer.
    [[nodiscard]] std::string chooseRenameName(QWidget*, const std::string& currentPath, bool folder) override {
        ++renamePrompts;
        lastRenameCurrentPath = currentPath;
        lastRenameFolder = folder;
        return renameName;
    }
    [[nodiscard]] bool confirmDelete(QWidget*, const QString& name, bool folder) override {
        ++deletePrompts;
        lastDeleteTargetPath = name.toStdString();
        lastDeleteFolder = folder;
        return confirmDeleteAnswer;
    }

    std::string openPath;
    OpenKind lastOpenKind = OpenKind::SolutionOrFile;
    int openCalls = 0;
    std::string saveAsPath;
    std::string lastSaveAsCurrentPath;
    std::size_t saveAsPrompts = 0;
    std::string newItemName;
    std::string lastNewItemDirectory;
    bool lastNewItemFolder = false;
    std::size_t newItemPrompts = 0;
    std::string renameName;
    std::string lastRenameCurrentPath;
    bool lastRenameFolder = false;
    std::size_t renamePrompts = 0;
    bool confirmDeleteAnswer = false;
    std::string lastDeleteTargetPath;
    bool lastDeleteFolder = false;
    std::size_t deletePrompts = 0;
    std::optional<NewProjectSpec> newProject;
    std::string lastNewProjectDefaultLocation;
    std::size_t newProjectPrompts = 0;
    bool discard = false;
    std::function<void()> onDiscard;
    int discardCalls = 0;
    QString lastDiscardError;
    bool rerun = true;
    int rerunCalls = 0;
    bool recover = false;
    int recoveryCalls = 0;
};

// What a rig changes from the defaults.
struct RigOptions {
    WorkbenchOptions workbench = [] {
        WorkbenchOptions defaults;
        defaults.clangd = false;
        defaults.git = false;
        return defaults;
    }();
    // Autosave on frame deactivation and on idle; off by default so the tests decide when saves run.
    bool autosave = false;
    // Keep project evaluation from consuming a scripted launcher in unrelated admission tests.
    bool disableProjectEvaluation = false;
    io::IProcessLauncher* launcher = nullptr;
    io::IFileSystem* fileSystem = nullptr;
};

/*
 * A Workbench and its MainWindow over files in `dir`: data/settings.json, data/trusted-roots.txt,
 * data/hot-exit, data/local-history.db, data/sessions, data/run-settings.txt, data/luals. Several rigs over one
 * directory share them, as two runs of FrostIDE would. clangd is off unless the options turn it on;
 * runs write to the Run console (no external console).
 */
class WorkbenchRig {
public:
    explicit WorkbenchRig(const WorkDir& dir, RigOptions options = {}) {
        io::IFileSystem& fs = options.fileSystem ? *options.fileSystem : io::realFileSystem();
        auto store = settings::SettingsStore::open(settings::builtinRegistry(), fs,
                                                   {dir.join("data/settings.json"), std::nullopt});
        EXPECT_TRUE(store.has_value()) << store.error().describe();
        settings = std::move(*store);
        EXPECT_TRUE(
            settings
                ->set(settings::Scope::Application, settings::key::saveAutosaveOnFrameDeactivation, options.autosave)
                .has_value());
        EXPECT_TRUE(settings->set(settings::Scope::Application, settings::key::saveAutosaveOnIdle, options.autosave)
                        .has_value());
        auto opened = project::TrustStore::open(fs, dir.join("data/trusted-roots.txt"));
        EXPECT_TRUE(opened.has_value()) << opened.error().describe();
        trust = std::move(*opened);

        WorkbenchOptions wb = std::move(options.workbench);
        if (wb.journalDirectory.empty()) {
            wb.journalDirectory = dir.join("data/hot-exit");
        }
        if (wb.historyPath.empty()) {
            wb.historyPath = dir.join("data/local-history.db");
        }
        if (wb.buildStatePath.empty()) {
            wb.buildStatePath = dir.join("data/active-configurations.txt");
        }
        if (wb.cmakeStatePath.empty()) {
            wb.cmakeStatePath = dir.join("data/cmake-profiles.txt");
        }
        if (wb.compileCommandsCache.empty()) {
            wb.compileCommandsCache = dir.join("data/compile-commands");
        }
        if (options.disableProjectEvaluation) {
            wb.compileCommandsCache.clear();
        }
        if (wb.sessionDirectory.empty()) {
            wb.sessionDirectory = dir.join("data/sessions");
        }
        if (wb.runStatePath.empty()) {
            wb.runStatePath = dir.join("data/run-settings.txt");
        }
        if (wb.luaStateDirectory.empty()) {
            wb.luaStateDirectory = dir.join("data/luals"); // LuaLS's log and meta, not the profile's.
        }
        if (!wb.luaSearch) {
            // No LuaLS unless the test asks for one, as before the pin (LUA-1): a test sets the LuaLS
            // path setting or luaSearch (WorkbenchLua.DefaultSearchStartsThePinnedLuaLs uses the
            // product's default search). Otherwise every trusted open would start a real LuaLS
            // beside the processes the test drives or scripts.
            wb.luaSearch = lang::LuaSearch{};
        }
        if (wb.applicationSettingsFile.empty()) {
            wb.applicationSettingsFile = dir.join("data/settings.json");
        }
        wb.runExternalConsole = false;
        wb.layout = input::simulatedLayout(input::SimulatedLayoutId::EnUs);
        wb.followActiveLayout = false;
        auto& launcher = options.launcher ? *options.launcher : io::realProcessLauncher();
        workbench = std::make_unique<Workbench>(
            WorkbenchServices{fs, launcher, systemClock(), *settings, *trust, prompts}, std::move(wb));
        window = std::make_unique<MainWindow>(workbench->environment());
        workbench->attach(*window);
    }
    ~WorkbenchRig() {
        // As main() does: the workbench shuts down while the window's documents are open.
        if (workbench) {
            workbench->shutdown();
        }
        window.reset();
        workbench.reset();
    }
    WorkbenchRig(const WorkbenchRig&) = delete ("owns the window");
    WorkbenchRig& operator=(const WorkbenchRig&) = delete ("owns the window");

    // Shows the window offscreen and waits until it is visible.
    void show() {
        window->show();
        waitFor([this] { return window->isVisible(); });
        QApplication::processEvents();
    }

    // The editor of tab `index` (inside the registry's banner view), or nullptr.
    [[nodiscard]] DocumentEditorView* view(int index = 0) const {
        const ui::IEditorView* tab = window->tabs().editor(index);
        return tab != nullptr ? workbench->editors().find(tab->path()) : nullptr;
    }
    // The navigation workbench (open, save, sessions, the restore prompt, external changes).
    [[nodiscard]] ui::Workbench& nav() const { return *workbench->navigation(); }

    ScriptedPrompts prompts;
    std::unique_ptr<settings::SettingsStore> settings;
    std::unique_ptr<project::TrustStore> trust;
    std::unique_ptr<Workbench> workbench;
    std::unique_ptr<MainWindow> window;
};

} // namespace frost::app::test
