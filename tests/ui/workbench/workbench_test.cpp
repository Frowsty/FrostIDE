// SPDX-License-Identifier: Apache-2.0
//
/*
 * Functional tests of frost::ui_workbench (ADR 0042), offscreen, over a MemFs (POSIX paths on every
 * platform) and fake editors: the File System view (listing, speed search, activation, refresh from
 * a watcher batch), Find Action (finds and runs an action, shows its shortcut, greys disabled ones),
 * Go to File ("mcpp" finds main.cpp, "main.cpp:3" jumps to line 3), Search Everywhere's sections,
 * Recent Files order, back/forward and the mouse buttons, the session round trip and the last
 * project at start-up, the hot-exit restore prompt calling SaveManager::restore, the merge view's
 * per-chunk choices, the large-file guard's notification on every open path, the banners, autosave on
 * focus loss, and (Windows) case variants opening one document.
 */

#include "frost/core/clock.hpp"
#include "frost/document/autosave.hpp"
#include "frost/document/load_service.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/nav/session.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/document_editor.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "../../../src/ui/workbench/src/file_tree_filter.hpp"
#include "shell/shell_rig.hpp"
#include "support/scratch_dir.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui::test {
namespace {

using namespace std::chrono_literals;

// A MemFs whose status() reports kMaxOpenFileBytes for the paths in `huge`, so the large-file guard
// refuses them from their status alone (nothing that large is ever allocated).
class HugeFileFs final : public io::IFileSystem {
public:
    explicit HugeFileFs(io::MemFs& inner) noexcept : m_inner(inner) {}
    ~HugeFileFs() = default;
    HugeFileFs(const HugeFileFs&) = delete ("wraps one MemFs");
    HugeFileFs& operator=(const HugeFileFs&) = delete ("wraps one MemFs");

    Result<std::string> readFile(std::string_view path) override { return m_inner.readFile(path); }
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_inner.writeFileAtomic(path, data);
    }
    Result<io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                          const io::SafeWriteOptions& options) override {
        auto result = m_inner.safeWrite(path, data, options);
        if (result && warnOnSave)
            result->warning = Error(ErrorCode::IoError, "recovery metadata needs attention");
        return result;
    }
    Result<io::FileStatus> status(std::string_view path) override {
        auto status = m_inner.status(path);
        if (status && huge.contains(std::string(path))) {
            status->size = document::kMaxOpenFileBytes;
        }
        return status;
    }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view path) override {
        return m_inner.listDirectory(path);
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        return m_inner.listDirectoryWithDiagnostics(path);
    }
    Result<void> createDirectories(std::string_view path) override { return m_inner.createDirectories(path); }
    Result<void> removeFile(std::string_view path) override { return m_inner.removeFile(path); }
    Result<void> removeDirectory(std::string_view path) override { return m_inner.removeDirectory(path); }
    Result<bool> exists(std::string_view path) override { return m_inner.exists(path); }
    Result<std::string> canonicalPath(std::string_view path) override { return m_inner.canonicalPath(path); }

    std::set<std::string> huge;
    bool warnOnSave = false;

private:
    io::MemFs& m_inner;
};

// One IDE instance: shell, documents and workbench over a shared file system.
struct Ide {
    Ide(io::IFileSystem& fs, ManualClock& clock, io::PathStyle style, nav::SessionStore* sessions,
        storage::HotExitJournal* journal = nullptr, document::SaveManagerOptions options = {},
        sched::IScheduler* scheduler = nullptr) {
        document::SaveManagerServices services;
        services.fs = &fs;
        services.clock = &clock;
        services.journal = journal;
        options.style = style;
        manager = std::make_unique<document::SaveManager>(services, options);
        documents = std::make_unique<nav::DocumentRegistry>(*manager, fs);
        editors = std::make_unique<DocumentEditorFactory>(rig.editors, *documents);
        ShellEnvironment environment{rig.registry, *rig.keymap, rig.settings, *editors};
        environment.followActiveLayout = false;
        window = std::make_unique<ShellWindow>(environment);
        autosave = std::make_unique<document::AutosaveController>(*manager, clock);
        WorkbenchServices workbenchServices{fs, *manager, *documents, *editors, clock};
        workbenchServices.scheduler = scheduler;
        workbenchServices.sessions = sessions;
        workbenchServices.autosave = autosave.get();
        workbench = std::make_unique<Workbench>(*window, std::move(workbenchServices));
    }
    Ide(const Ide&) = delete ("owns a window");
    Ide& operator=(const Ide&) = delete ("owns a window");
    ~Ide() {
        workbench.reset();
        window.reset();
    }

    [[nodiscard]] std::vector<std::string> tabPaths() const {
        std::vector<std::string> paths;
        for (int i = 0; i < window->tabs().count(); ++i) {
            paths.push_back(window->tabs().editor(i)->path());
        }
        return paths;
    }
    [[nodiscard]] std::string activePath() const {
        const IEditorView* editor = window->tabs().currentEditor();
        return editor != nullptr ? editor->path() : std::string();
    }
    [[nodiscard]] int notificationsTitled(const QString& part) const {
        int count = 0;
        for (const NotificationEntry& entry : window->notifications().log()) {
            count += entry.notification.title.contains(part) ? 1 : 0;
        }
        return count;
    }
    [[nodiscard]] const Notification* lastNotificationTitled(const QString& part) const {
        const auto& log = window->notifications().log();
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->notification.title.contains(part)) {
                return &it->notification;
            }
        }
        return nullptr;
    }

    ShellRig rig;
    std::unique_ptr<document::SaveManager> manager;
    std::unique_ptr<nav::DocumentRegistry> documents;
    std::unique_ptr<DocumentEditorFactory> editors;
    std::unique_ptr<ShellWindow> window;
    std::unique_ptr<document::AutosaveController> autosave;
    std::unique_ptr<Workbench> workbench;
};

// The index of the first item whose text is `text`, or -1.
int itemIndex(const ListPopup& popup, const QString& text) {
    const auto& items = popup.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].text == text) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

class WorkbenchTest : public ::testing::Test {
protected:
    WorkbenchTest() : memFs(clock, io::PathStyle::Posix), fs(memFs) {
        file("/proj/.gitignore", "*.log\n");
        file("/proj/src/main.cpp", "int a;\nint b;\nint c;\nint d;\n");
        file("/proj/src/util.cpp", "// util\n");
        file("/proj/README.md", "# proj\n");
        file("/proj/debug.log", "ignored\n");
        file("/proj/build/out.o", "ignored\n");
        file("/proj/.git/HEAD", "ref\n");
    }

    void file(const std::string& path, std::string_view content) {
        ASSERT_TRUE(memFs.createDirectories(path.substr(0, path.rfind('/'))).has_value()) << path;
        ASSERT_TRUE(memFs.writeFileAtomic(path, content).has_value()) << path;
    }
    // Another program changes `path`, one tick later.
    void writeExternally(const std::string& path, std::string_view content) {
        clock.advance(10ms);
        file(path, content);
    }

    Ide& ide(nav::SessionStore* sessions = nullptr, document::SaveManagerOptions options = {}) {
        m_ide = std::make_unique<Ide>(fs, clock, io::PathStyle::Posix, sessions, nullptr, options);
        return *m_ide;
    }
    void openProject(Ide& target) {
        auto opened = target.workbench->openProject("/proj");
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
    }

    static io::FileChangeBatch modified(const std::string& path) {
        io::FileChangeBatch batch;
        batch.changes.push_back(io::FileChange{io::FileChangeKind::Modified, path, {}, io::FileType::Regular});
        batch.sequence = 1;
        return batch;
    }

    static void edit(document::FileDocument& document, std::uint64_t begin, std::uint64_t end, std::string_view text) {
        auto changed =
            document.document().replace(text::ByteRange{ByteOffset{begin}, ByteOffset{end}}, text, text::EditContext{});
        ASSERT_TRUE(changed) << changed.error().describe();
        document.document().breakCoalescing();
    }

    ManualClock clock{IClock::SteadyTime{std::chrono::hours(1)}, IClock::SystemTime{std::chrono::hours(24 * 365 * 50)}};
    io::MemFs memFs;
    HugeFileFs fs;
    std::unique_ptr<Ide> m_ide;

    void TearDown() override { m_ide.reset(); }
};

TEST_F(WorkbenchTest, FileSystemViewListsSpeedSearchesOpensAndRefreshes) {
    Ide& app = ide();
    openProject(app);
    app.window->toolWindows().show(kFileSystemToolWindow, false);
    FileSystemView* view = app.workbench->fileSystemView();
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(qobject_cast<QStandardItemModel*>(view->tree()->model()), nullptr);
    // .gitignore'd debug.log, the default-excluded build/ and the skipped .git/ are not listed.
    EXPECT_EQ(view->visibleNames(), (QStringList{"proj", "src", ".gitignore", "README.md"}));
    EXPECT_EQ(view->setSpeedSearch(QStringLiteral("util")), 1);
    EXPECT_EQ(view->selectedPath(), "/proj/src/util.cpp");
    EXPECT_EQ(view->visibleNames(), (QStringList{"proj", "src", "util.cpp"}));
    view->activateSelected();
    EXPECT_EQ(app.activePath(), "/proj/src/util.cpp");
    view->setSpeedSearch(QString());
    // A watcher batch for a new file rescans the folder.
    file("/proj/notes.txt", "new\n");
    io::FileChangeBatch batch;
    batch.changes.push_back(io::FileChange{io::FileChangeKind::Created, "/proj/notes.txt", {}, io::FileType::Regular});
    app.workbench->applyFileChanges(batch);
    EXPECT_TRUE(view->visibleNames().contains(QStringLiteral("notes.txt")));
}

TEST_F(WorkbenchTest, OldScanIsDiscardedAfterReopeningTheSameRoot) {
    sched::SimExecutor sim;
    Ide app(fs, clock, io::PathStyle::Posix, nullptr, nullptr, {}, &sim);
    openProject(app);
    sim.runUntilIdle(); // The first scan has completed, but its GUI timer has not published it.
    ASSERT_TRUE(app.workbench->closeProject());
    file("/proj/new.cpp", "new");
    openProject(app);
    QTimer* timer = app.workbench->findChild<QTimer*>(QStringLiteral("projectScanTimer"));
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(app.workbench->projectFiles().fileCount(), 0U);
    sim.runUntilIdle(); // The queued scan belongs to the new project generation.
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_TRUE(app.workbench->projectFiles().find("/proj/new.cpp").has_value());
}

TEST_F(WorkbenchTest, SolutionOnlyCandidatesArriveBeforeTheScanFinishes) {
    const bool windows = io::kNativePathStyle == io::PathStyle::Windows;
    const std::string root = windows ? "C:\\proj" : "/proj";
    const std::string solution = root + (windows ? "\\demo.sln" : "/demo.sln");
    const std::string outside = windows ? "C:\\outside\\shared.h" : "/outside/shared.h";
    io::MemFs nativeFs(clock, io::kNativePathStyle);
    HugeFileFs wrapped(nativeFs);
    ASSERT_TRUE(nativeFs.createDirectories(root).has_value());
    ASSERT_TRUE(
        nativeFs
            .writeFileAtomic(solution,
                             "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                             "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"Items\", \"Items\", "
                             "\"{00000000-0000-4000-8000-000000000001}\"\n"
                             "ProjectSection(SolutionItems) = preProject\n../outside/shared.h = ../outside/shared.h\n"
                             "EndProjectSection\nEndProject\nGlobal\nEndGlobal\n")
            .has_value());
    sched::SimExecutor sim(sched::SimOptions{.seed = 1});
    Ide app(wrapped, clock, io::kNativePathStyle, nullptr, nullptr, {}, &sim);
    auto opened = app.workbench->openProject(solution);
    ASSERT_TRUE(opened.has_value()) << opened.error().describe();
    EXPECT_TRUE(app.workbench->projectFiles().candidates().empty());
    QTimer* timer = app.workbench->findChild<QTimer*>(QStringLiteral("projectScanTimer"));
    ASSERT_NE(timer, nullptr);
    for (int i = 0; i < 100 && app.workbench->projectFiles().candidates().empty(); ++i) {
        ASSERT_TRUE(sim.step());
        ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    }
    const auto& candidates = app.workbench->projectFiles().candidates();
    EXPECT_NE(std::find(candidates.begin(), candidates.end(), outside), candidates.end());
    EXPECT_TRUE(app.workbench->scanPending());
    sim.runUntilIdle();
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_NE(std::find(app.workbench->projectFiles().candidates().begin(),
                        app.workbench->projectFiles().candidates().end(), outside),
              app.workbench->projectFiles().candidates().end());
}

TEST_F(WorkbenchTest, RefreshSolutionKeepsDirtyTabsAndNavigation) {
    const bool windows = io::kNativePathStyle == io::PathStyle::Windows;
    const std::string root = windows ? "C:\\proj" : "/proj";
    const std::string solutionPath = root + (windows ? "\\demo.sln" : "/demo.sln");
    const std::string sourcePath = root + (windows ? "\\main.cpp" : "/main.cpp");
    io::MemFs nativeFs(clock, io::kNativePathStyle);
    ASSERT_TRUE(nativeFs.createDirectories(root));
    ASSERT_TRUE(nativeFs.writeFileAtomic(
        solutionPath, "Microsoft Visual Studio Solution File, Format Version 12.00\nGlobal\nEndGlobal\n"));
    ASSERT_TRUE(nativeFs.writeFileAtomic(sourcePath, "int main() {}\n"));
    Ide app(nativeFs, clock, io::kNativePathStyle, nullptr);
    auto model = project::SolutionModel::load(nativeFs, solutionPath);
    ASSERT_TRUE(model);
    ASSERT_TRUE(app.workbench->openSolution(*model));
    ASSERT_TRUE(app.workbench->openFile(sourcePath));
    auto* document = app.documents->find(sourcePath);
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 0, "// unsaved\n");
    app.workbench->history().recordJump(nav::Place{sourcePath, 1, 1});
    const auto history = std::vector<nav::Place>(app.workbench->history().backStack().begin(),
                                                 app.workbench->history().backStack().end());
    const auto tabs = app.tabPaths();
    const auto recent = std::vector<std::string>(app.workbench->recentFiles().paths().begin(),
                                                 app.workbench->recentFiles().paths().end());
    ASSERT_TRUE(app.workbench->refreshSolution(*model));
    EXPECT_EQ(app.tabPaths(), tabs);
    EXPECT_EQ(app.activePath(), sourcePath);
    EXPECT_TRUE(document->dirty());
    const auto disk = nativeFs.readFile(sourcePath);
    ASSERT_TRUE(disk);
    EXPECT_EQ(*disk, "int main() {}\n");
    EXPECT_EQ(std::vector<std::string>(app.workbench->recentFiles().paths().begin(),
                                       app.workbench->recentFiles().paths().end()),
              recent);
    EXPECT_EQ(std::vector<nav::Place>(app.workbench->history().backStack().begin(),
                                      app.workbench->history().backStack().end()),
              history);
    EXPECT_EQ(app.workbench->projectRoot(), root);
}

TEST_F(WorkbenchTest, FileTreeFilterKeepsMatchingAncestorsAndSortedRows) {
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    nav::ProjectFiles files("/proj");
    files.setScan(*scan);
    auto filter = workbench_detail::filterFileTree(files, "util", {});
    ASSERT_TRUE(filter.has_value());
    EXPECT_EQ(filter->matches, 1);
    const auto util = files.find("/proj/src/util.cpp");
    const auto src = files.find("/proj/src");
    ASSERT_TRUE(util.has_value());
    ASSERT_TRUE(src.has_value());
    EXPECT_TRUE(filter->visible[0]);
    EXPECT_TRUE(filter->visible[*src]);
    EXPECT_TRUE(filter->visible[*util]);
    EXPECT_EQ(filter->first, *util);
    EXPECT_EQ(filter->rows[*util], 0U);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(workbench_detail::filterFileTree(files, "util", stop.get_token()).has_value());
}

TEST_F(WorkbenchTest, TypingQueuesSearchAndPublishesItsResult) {
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    auto files = std::make_shared<nav::ProjectFiles>("/proj");
    files->setScan(*scan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(files);
    view.searchField()->setText(QStringLiteral("util"));
    EXPECT_GT(sim.readyCount(), 0U);
    EXPECT_TRUE(view.tree()->isHidden());
    sim.runUntilIdle();
    QTimer* timer = view.findChild<QTimer*>(QStringLiteral("projectFilterTimer"));
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(view.selectedPath(), "/proj/src/util.cpp");
    EXPECT_EQ(view.visibleNames(), (QStringList{"proj", "src", "util.cpp"}));
}

TEST_F(WorkbenchTest, PendingQueryAndProjectCannotActivateOrNavigateOldSelection) {
    file("/other/main.cpp", "other");
    sched::SimExecutor sim;
    auto oldScan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    auto newScan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/other")).join();
    ASSERT_TRUE(oldScan && newScan);
    auto oldFiles = std::make_shared<nav::ProjectFiles>("/proj");
    auto newFiles = std::make_shared<nav::ProjectFiles>("/other");
    oldFiles->setScan(*oldScan);
    newFiles->setScan(*newScan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(oldFiles);
    ASSERT_TRUE(view.select("/proj/src/main.cpp"));
    int activations = 0;
    QObject::connect(&view, &FileSystemView::fileActivated, &view, [&](const QString&) { ++activations; });
    view.searchField()->setText(QStringLiteral("util"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(view.searchField(), &enter);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(view.searchField(), &down);
    EXPECT_EQ(activations, 0);
    EXPECT_EQ(view.selectedPath(), "/proj/src/main.cpp");
    view.setFiles(newFiles);
    QCoreApplication::sendEvent(view.searchField(), &enter);
    EXPECT_EQ(activations, 0);
}

TEST_F(WorkbenchTest, ExpansionRestorationIsBatched) {
    for (int i = 0; i < 150; ++i)
        file("/proj/d" + std::to_string(i) + "/leaf", "");
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    auto files = std::make_shared<nav::ProjectFiles>("/proj");
    files->setScan(*scan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(files);
    for (int i = 0; i < 150; ++i)
        ASSERT_TRUE(view.select("/proj/d" + std::to_string(i) + "/leaf"));
    view.setFiles(files);
    EXPECT_LT(view.visibleNames().count(QStringLiteral("leaf")), 150);
    QTimer* timer = view.findChild<QTimer*>(QStringLiteral("projectFilterTimer"));
    ASSERT_NE(timer, nullptr);
    const auto before = view.visibleNames().count(QStringLiteral("leaf"));
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_LE(view.visibleNames().count(QStringLiteral("leaf")) - before, 64);
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(view.visibleNames().count(QStringLiteral("leaf")), 150);
}

TEST_F(WorkbenchTest, ReplacedUnpublishedSnapshotIsRetiredOnTheScheduler) {
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    auto base = std::make_shared<nav::ProjectFiles>("/proj");
    base->setScan(*scan);
    bool retired = false;
    auto unpublished =
        std::shared_ptr<nav::ProjectFiles>(new nav::ProjectFiles("/proj"), [&](nav::ProjectFiles* files) {
            retired = true;
            delete files;
        });
    unpublished->setScan(*scan);
    auto replacement = std::make_shared<nav::ProjectFiles>("/proj");
    replacement->setScan(*scan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(base);
    view.searchField()->setText(QStringLiteral("main"));
    view.setFiles(unpublished);
    unpublished.reset();
    view.setFiles(replacement);
    EXPECT_FALSE(retired);
    sim.runUntilIdle();
    EXPECT_TRUE(retired);
}

TEST_F(WorkbenchTest, FilteredPublicationDiscardsAnObsoleteQuery) {
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    auto files = std::make_shared<nav::ProjectFiles>("/proj");
    files->setScan(*scan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(files);
    EXPECT_EQ(view.setSpeedSearch(QStringLiteral("util")), 1);
    sim.runUntilIdle();
    EXPECT_EQ(view.setSpeedSearch(QStringLiteral("main")), 1);
    QTimer* timer = view.findChild<QTimer*>(QStringLiteral("projectFilterTimer"));
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    sim.runUntilIdle();
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(view.selectedPath(), "/proj/src/main.cpp");
    EXPECT_EQ(view.visibleNames(), (QStringList{"proj", "src", "main.cpp"}));
    view.setSpeedSearch(QString());
    EXPECT_EQ(view.selectedPath(), "/proj/src/main.cpp");
}

TEST_F(WorkbenchTest, FilteredPublicationDropsOldProjectAndClose) {
    file("/other/main.cpp", "other");
    sched::SimExecutor sim;
    auto oldScan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    auto newScan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/other")).join();
    ASSERT_TRUE(oldScan.has_value());
    ASSERT_TRUE(newScan.has_value());
    auto oldFiles = std::make_shared<nav::ProjectFiles>("/proj");
    auto newFiles = std::make_shared<nav::ProjectFiles>("/other");
    oldFiles->setScan(*oldScan);
    newFiles->setScan(*newScan);
    FileSystemView view(nullptr, &sim);
    view.setFiles(oldFiles);
    EXPECT_EQ(view.setSpeedSearch(QStringLiteral("main")), 1);
    sim.runUntilIdle(); // Old query is complete but not yet published.
    view.setFiles(newFiles);
    EXPECT_TRUE(view.tree()->isHidden());
    QTimer* timer = view.findChild<QTimer*>(QStringLiteral("projectFilterTimer"));
    ASSERT_NE(timer, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    sim.runUntilIdle();
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(view.selectedPath(), "/other/main.cpp");
    view.setFiles(oldFiles);
    sim.runUntilIdle();
    view.setFiles(std::make_shared<nav::ProjectFiles>());
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    sim.runUntilIdle();
    ASSERT_TRUE(view.visibleNames().isEmpty());
    EXPECT_TRUE(view.tree()->isHidden());
}

TEST_F(WorkbenchTest, FileTreePublicationDoesNotColorEveryNode) {
    for (int i = 0; i < 300; ++i) {
        file("/proj/hidden/file" + std::to_string(i) + ".cpp", "");
    }
    sched::SimExecutor sim;
    auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value());
    auto files = std::make_shared<nav::ProjectFiles>("/proj");
    files->setScan(*scan);
    FileSystemView view;
    int colorQueries = 0;
    view.setFileColorProvider([&](std::string_view) -> std::optional<QColor> {
        ++colorQueries;
        return std::nullopt;
    });
    view.setFiles(files);
    EXPECT_LT(colorQueries, 300);
    EXPECT_TRUE(view.select("/proj/hidden/file299.cpp"));
    EXPECT_EQ(view.selectedPath(), "/proj/hidden/file299.cpp");
}

TEST_F(WorkbenchTest, FindActionFindsRunsShowsShortcutsAndGreysDisabledActions) {
    Ide& app = ide();
    int runs = 0;
    app.rig.addCounted("test.sayHello", "Say Hello", runs);
    ListPopup* popup = app.workbench->showFindAction(QStringLiteral("say hello"));
    ASSERT_NE(popup, nullptr);
    const int hello = itemIndex(*popup, QStringLiteral("Say Hello"));
    ASSERT_GE(hello, 0);
    popup->setCurrentItem(hello);
    popup->acceptCurrent();
    EXPECT_EQ(runs, 1);

    ListPopup* shortcuts = app.workbench->showFindAction(QStringLiteral("go to file"));
    const int gotoFile = itemIndex(*shortcuts, QStringLiteral("Go to File..."));
    ASSERT_GE(gotoFile, 0);
    EXPECT_TRUE(shortcuts->items()[static_cast<std::size_t>(gotoFile)].detail.contains(QStringLiteral("Ctrl+Shift+N")))
        << shortcuts->items()[static_cast<std::size_t>(gotoFile)].detail.toStdString();
    // Running it opens Go to File.
    shortcuts->setCurrentItem(gotoFile);
    shortcuts->acceptCurrent();
    ASSERT_NE(app.window->activePopup(), nullptr);
    EXPECT_EQ(app.window->activePopup()->objectName(), QStringLiteral("goToFilePopup"));

    // Without an editor, Save Document is listed greyed and does not run.
    ListPopup* disabled = app.workbench->showFindAction(QStringLiteral("save document"));
    const int save = itemIndex(*disabled, QStringLiteral("Save Document"));
    ASSERT_GE(save, 0);
    EXPECT_FALSE(disabled->items()[static_cast<std::size_t>(save)].enabled);
}

TEST_F(WorkbenchTest, FindActionOffersTheSettingsFiles) {
    Ide& app = ide();
    openProject(app);
    ListPopup* popup = app.workbench->showFindAction(QStringLiteral("project settings file"));
    const int shared = itemIndex(*popup, QStringLiteral("Open Project Settings File"));
    ASSERT_GE(shared, 0);
    popup->setCurrentItem(shared);
    popup->acceptCurrent();
    EXPECT_EQ(app.activePath(), "/proj/.frost/settings.json");
    EXPECT_EQ(memFs.readFile("/proj/.frost/settings.json").value_or(""), "{}\n");
    EXPECT_TRUE(memFs.exists("/proj/.frost/.gitignore").value_or(false));
    ListPopup* personal = app.workbench->showFindAction(QStringLiteral("personal project settings"));
    EXPECT_GE(itemIndex(*personal, QStringLiteral("Open Personal Project Settings File")), 0);
    ListPopup* application = app.workbench->showFindAction(QStringLiteral("application settings"));
    EXPECT_GE(itemIndex(*application, QStringLiteral("Open Application Settings File")), 0);
}

TEST_F(WorkbenchTest, GoToFileMatchesCamelHumpsAndJumpsToALine) {
    file("/proj/docs/manual.md", "m\n");
    Ide& app = ide();
    openProject(app);
    ListPopup* popup = app.workbench->showGoToFile();
    ASSERT_NE(popup, nullptr);
    popup->setFilterText(QStringLiteral("mcpp"));
    ASSERT_FALSE(popup->items().empty());
    EXPECT_EQ(popup->items().front().text, QStringLiteral("main.cpp"));
    popup->setFilterText(QStringLiteral("main.cpp:3"));
    ASSERT_FALSE(popup->items().empty());
    EXPECT_EQ(popup->items()[static_cast<std::size_t>(popup->currentItem())].text, QStringLiteral("main.cpp"));
    popup->acceptCurrent();
    EXPECT_EQ(app.activePath(), "/proj/src/main.cpp");
    ASSERT_NE(app.rig.editors.last, nullptr);
    EXPECT_EQ(app.window->tabs().currentEditor()->status().line, 3);
    // A path segment narrows by folder.
    ListPopup* segments = app.workbench->showGoToFile(QStringLiteral("src/u"));
    ASSERT_FALSE(segments->items().empty());
    EXPECT_EQ(segments->items().front().text, QStringLiteral("util.cpp"));
}

TEST_F(WorkbenchTest, SearchEverywhereShowsFilesAndActions) {
    file("/proj/save_notes.txt", "s\n");
    Ide& app = ide();
    openProject(app);
    ListPopup* popup = app.workbench->showSearchEverywhere(QStringLiteral("save"));
    ASSERT_NE(popup, nullptr);
    const auto& items = popup->items();
    ASSERT_GE(items.size(), 4U);
    EXPECT_TRUE(items[0].header);
    EXPECT_EQ(items[0].text, QStringLiteral("Files"));
    EXPECT_EQ(items[1].text, QStringLiteral("save_notes.txt"));
    EXPECT_GE(itemIndex(*popup, QStringLiteral("Actions")), 2);
    EXPECT_GE(itemIndex(*popup, QStringLiteral("Save All")), 3);
    EXPECT_EQ(popup->currentItem(), 1); // The header is skipped.
    popup->acceptCurrent();
    EXPECT_EQ(app.activePath(), "/proj/save_notes.txt");
}

TEST_F(WorkbenchTest, RecentFilesListsTheMostRecentFirst) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md").has_value());
    ASSERT_TRUE(app.workbench->openFile("/proj/src/util.cpp").has_value());
    ASSERT_TRUE(app.workbench->openFile("/proj/src/main.cpp").has_value());
    ListPopup* popup = app.workbench->showRecentFiles();
    ASSERT_NE(popup, nullptr);
    ASSERT_EQ(popup->items().size(), 3U);
    EXPECT_EQ(popup->items()[0].text, QStringLiteral("main.cpp"));
    EXPECT_EQ(popup->items()[1].text, QStringLiteral("util.cpp"));
    EXPECT_EQ(popup->items()[2].text, QStringLiteral("README.md"));
    EXPECT_EQ(popup->currentItem(), 1); // The file before the current one.
    popup->acceptCurrent();
    EXPECT_EQ(app.activePath(), "/proj/src/util.cpp");
}

TEST_F(WorkbenchTest, BackAndForwardRestorePlaces) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md").has_value());
    ASSERT_TRUE(app.workbench->openFile("/proj/src/main.cpp", LinePosition{4, 2}).has_value());
    EXPECT_TRUE(app.workbench->goBack());
    EXPECT_EQ(app.activePath(), "/proj/README.md");
    EXPECT_TRUE(app.workbench->goForward());
    EXPECT_EQ(app.activePath(), "/proj/src/main.cpp");
    EXPECT_EQ(app.window->tabs().currentEditor()->status().line, 4);
    // A big caret move inside a file is a place too.
    app.rig.editors.last->goToLine(40, 1);
    EXPECT_TRUE(app.workbench->goBack());
    EXPECT_EQ(app.window->tabs().currentEditor()->status().line, 4);
    // The mouse's back button.
    QMouseEvent back(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::BackButton, Qt::BackButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&app.window->tabs(), &back);
    EXPECT_EQ(app.activePath(), "/proj/README.md");
    // Ctrl+Shift+E lists the places.
    ListPopup* locations = app.workbench->showRecentLocations();
    ASSERT_NE(locations, nullptr);
    EXPECT_FALSE(locations->items().empty());
}

TEST_F(WorkbenchTest, SessionRoundTripAndTheLastProjectAtStartUp) {
    nav::SessionStore store(fs, "/data/sessions");
    {
        Ide& app = ide(&store);
        openProject(app);
        ASSERT_TRUE(app.workbench->openFile("/proj/README.md").has_value());
        ASSERT_TRUE(app.workbench->openFile("/proj/src/util.cpp").has_value());
        ASSERT_TRUE(app.workbench->openFile("/proj/src/main.cpp", LinePosition{3, 1}).has_value());
        EditorTabs& tabs = app.window->tabs();
        tabs.setPinned(tabs.indexOf("/proj/src/util.cpp"), true);
        tabs.activate(tabs.indexOf("/proj/src/util.cpp"));
        app.window->toolWindows().show(kFileSystemToolWindow, false);
        const std::vector<std::string> before = app.tabPaths();
        app.workbench->closeProject();
        EXPECT_EQ(tabs.count(), 0);
        // Reopening the project restores it.
        openProject(app);
        EXPECT_EQ(app.tabPaths(), before);
        EXPECT_EQ(app.activePath(), "/proj/src/util.cpp");
        EXPECT_TRUE(tabs.isPinned(tabs.indexOf("/proj/src/util.cpp")));
        EXPECT_EQ(tabs.editor(tabs.indexOf("/proj/src/main.cpp"))->status().line, 3);
        EXPECT_TRUE(app.window->toolWindows().isVisible(kFileSystemToolWindow));
        EXPECT_EQ(app.workbench->recentFiles().paths().front(), "/proj/src/util.cpp");
    }
    // A new start opens the last project with its session.
    m_ide.reset();
    Ide& next = ide(&store);
    EXPECT_TRUE(next.workbench->restoreLastProject());
    EXPECT_EQ(next.workbench->projectRoot(), "/proj");
    EXPECT_EQ(next.window->tabs().count(), 3);
    EXPECT_EQ(next.activePath(), "/proj/src/util.cpp");
    m_ide.reset(); // Before the store.
}

TEST_F(WorkbenchTest, SplitSessionRestoresGroupsAndEmptyActiveGroup) {
    nav::SessionStore store(fs, "/data/sessions");
    Ide& app = ide(&store);
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md").has_value());
    EditorTabs& tabs = app.window->tabs();
    tabs.currentEditor()->goToLine(2, 1);
    ASSERT_TRUE(tabs.split(Qt::Horizontal));
    tabs.currentEditor()->goToLine(3, 1);
    ASSERT_TRUE(app.workbench->openFile("/proj/src/util.cpp").has_value());
    ASSERT_TRUE(tabs.split(Qt::Vertical));
    ASSERT_TRUE(tabs.close(tabs.currentIndex()));
    ASSERT_EQ(tabs.groupCount(), 3);
    ASSERT_EQ(tabs.count(), 0);
    app.workbench->closeProject();
    openProject(app);
    EXPECT_EQ(tabs.groupCount(), 3);
    EXPECT_EQ(tabs.count(), 0);
    const auto groups = tabs.groups();
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(groups[0]->editor(0)->path(), "/proj/README.md");
    EXPECT_EQ(groups[0]->editor(0)->status().line, 2);
    EXPECT_EQ(groups[1]->editor(groups[1]->indexOf("/proj/README.md"))->status().line, 3);
    EXPECT_GE(groups[1]->indexOf("/proj/src/util.cpp"), 0);
    m_ide.reset(); // Before the local session store.
}

TEST_F(WorkbenchTest, RestorePromptCallsSaveManagerRestore) {
    frost::test::ScratchDir scratch("workbench-hot-exit");
    const std::string journals = scratch.join("journals");
    {
        auto journal = storage::HotExitJournal::create(journals, clock);
        ASSERT_TRUE(journal.has_value()) << journal.error().describe();
        auto first = std::make_unique<Ide>(fs, clock, io::PathStyle::Posix, nullptr, &*journal);
        ASSERT_TRUE(first->workbench->openFile("/proj/README.md").has_value());
        document::FileDocument* document = first->documents->find("/proj/README.md");
        ASSERT_NE(document, nullptr);
        edit(*document, 0, 0, "unsaved ");
        EXPECT_EQ(first->manager->journalDirty().written, 1U);
        first.reset(); // The process dies: no closeJournal().
    }
    auto journal = storage::HotExitJournal::create(journals, clock);
    ASSERT_TRUE(journal.has_value());
    auto scan = storage::findRecoverableJournals(journals, 500ms, journal->path());
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    auto second = std::make_unique<Ide>(fs, clock, io::PathStyle::Posix, nullptr, &*journal);
    EXPECT_EQ(second->workbench->offerRestore(std::move(*scan)), 1U);
    const Notification* prompt = second->lastNotificationTitled(QStringLiteral("Restore 1 unsaved files?"));
    ASSERT_NE(prompt, nullptr);
    EXPECT_TRUE(prompt->title.startsWith(QStringLiteral("FrostIDE closed unexpectedly.")));
    ASSERT_EQ(prompt->actions.size(), 2U);
    EXPECT_EQ(prompt->actions[0].label, QStringLiteral("Restore"));
    prompt->actions[0].run();
    EXPECT_FALSE(second->workbench->restorePending());
    document::FileDocument* restored = second->documents->find("/proj/README.md");
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->dirty());
    EXPECT_EQ(restored->document().committedText().text(), "unsaved # proj\n");
    EXPECT_EQ(second->activePath(), "/proj/README.md");
    second.reset();
}

TEST_F(WorkbenchTest, MergeViewAppliesPerChunkChoicesAndResolves) {
    file("/proj/merge.txt", "a\nb\nc\n");
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/merge.txt").has_value());
    document::FileDocument* document = app.documents->find("/proj/merge.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "X");
    writeExternally("/proj/merge.txt", "Y\nb\nC\n");
    app.workbench->applyFileChanges(modified("/proj/merge.txt"));
    ASSERT_NE(document->pendingMerge(), nullptr);
    const auto banners = app.editors->banners("/proj/merge.txt");
    ASSERT_EQ(banners.size(), 1U);
    EXPECT_EQ(banners[0]->kind(), BannerKind::MergePending);
    // The one external-change prompt: Keep my version, Take disk version, Compare... (the merge view).
    const Notification* changed = app.lastNotificationTitled(QStringLiteral("changed on disk"));
    ASSERT_NE(changed, nullptr);
    ASSERT_EQ(changed->actions.size(), 3U);
    EXPECT_EQ(changed->actions[0].label, QStringLiteral("Keep my version"));
    EXPECT_EQ(changed->actions[1].label, QStringLiteral("Take disk version"));
    EXPECT_EQ(changed->actions[2].label, QStringLiteral("Compare..."));
    changed->actions[2].run();
    MergeView* view = app.window->findChild<MergeView*>(QStringLiteral("mergeView"));
    ASSERT_NE(view, nullptr);
    // Three aligned panes: every pane has the same rows.
    EXPECT_EQ(view->layout().map(nav::MergePane::Yours).rowCount(),
              view->layout().map(nav::MergePane::Disk).rowCount());
    ASSERT_EQ(view->changes().size(), 2U); // The conflict on line 1, the disk's change on line 3.
    EXPECT_EQ(view->unresolvedCount(), 1U);
    ASSERT_TRUE(view->acceptRight(view->changes()[0]).has_value());
    ASSERT_TRUE(view->acceptLeft(view->changes()[1]).has_value());
    EXPECT_EQ(view->unresolvedCount(), 0U);
    ASSERT_TRUE(view->apply().has_value());
    EXPECT_EQ(document->document().committedText().text(), "Y\nb\nc\n");
    EXPECT_EQ(document->pendingMerge(), nullptr);
    EXPECT_EQ(document->diskState(), document::DiskState::InSync);
    EXPECT_TRUE(app.editors->banners("/proj/merge.txt").empty());
}

TEST_F(WorkbenchTest, RefusedHugeFileIsNotifiedOnEveryOpenPath) {
    file("/proj/huge.bin", "tiny on disk, huge by status\n");
    fs.huge.insert("/proj/huge.bin");
    Ide& app = ide();
    openProject(app);
    const QString tooLarge = QStringLiteral("is too large to open");
    // The File System view.
    app.window->toolWindows().show(kFileSystemToolWindow, false);
    ASSERT_TRUE(app.workbench->fileSystemView()->select("/proj/huge.bin"));
    app.workbench->fileSystemView()->activateSelected();
    EXPECT_EQ(app.notificationsTitled(tooLarge), 1);
    // Go to File.
    ListPopup* gotoFile = app.workbench->showGoToFile(QStringLiteral("huge"));
    ASSERT_FALSE(gotoFile->items().empty());
    gotoFile->acceptCurrent();
    EXPECT_EQ(app.notificationsTitled(tooLarge), 2);
    // Ctrl+E (the file was recent when it was smaller).
    app.workbench->recentFiles().touch("/proj/huge.bin");
    ListPopup* recent = app.workbench->showRecentFiles();
    ASSERT_NE(recent, nullptr);
    recent->setCurrentItem(0);
    recent->acceptCurrent();
    EXPECT_EQ(app.notificationsTitled(tooLarge), 3);
    // Session restore.
    nav::SessionState state;
    state.root = "/proj";
    state.tabs.push_back(nav::SessionTab{"/proj/huge.bin", false, 1, 1, 0});
    state.active = 0;
    app.workbench->applySession(state);
    EXPECT_EQ(app.notificationsTitled(tooLarge), 4);
    EXPECT_EQ(app.window->tabs().count(), 0);
    EXPECT_EQ(app.manager->documents().size(), 0U);
}

TEST_F(WorkbenchTest, BannersShowLongLinesAndOutOfDateFiles) {
    file("/proj/long.txt", std::string(50, 'x') + "\n");
    file("/proj/grow.txt", "small\n");
    document::SaveManagerOptions options;
    options.load.maxEditableLineBytes = 10;
    Ide& app = ide(nullptr, options);
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/long.txt").has_value());
    auto banners = app.editors->banners("/proj/long.txt");
    ASSERT_EQ(banners.size(), 1U);
    EXPECT_EQ(banners[0]->kind(), BannerKind::LongLine);
    EXPECT_TRUE(app.window->tabs().currentEditor()->status().readOnly);

    ASSERT_TRUE(app.workbench->openFile("/proj/grow.txt").has_value());
    EXPECT_TRUE(app.editors->banners("/proj/grow.txt").empty());
    writeExternally("/proj/grow.txt", "bigger now\n");
    fs.huge.insert("/proj/grow.txt");
    app.workbench->applyFileChanges(modified("/proj/grow.txt"));
    banners = app.editors->banners("/proj/grow.txt");
    ASSERT_EQ(banners.size(), 1U);
    EXPECT_EQ(banners[0]->kind(), BannerKind::OutOfDate);
    QStringList actions;
    for (const QPushButton* button : banners[0]->findChildren<QPushButton*>(QStringLiteral("bannerAction"))) {
        actions.push_back(button->text());
    }
    EXPECT_EQ(actions, (QStringList{"Reload", "Overwrite"}));
    EXPECT_EQ(app.notificationsTitled(QStringLiteral("is too large to reload")), 1);
}

TEST_F(WorkbenchTest, FocusLossAutosavesAndSaveAllSaves) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md").has_value());
    document::FileDocument* document = app.documents->find("/proj/README.md");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 0, "A ");
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(app.window.get(), &deactivate);
    EXPECT_FALSE(document->dirty());
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "A # proj\n");
    edit(*document, 0, 0, "B ");
    const document::SaveAllResult saved = app.workbench->saveAll();
    EXPECT_EQ(saved.saved.size(), 1U);
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "B A # proj\n");
    // Save All is Ctrl+S.
    EXPECT_EQ(app.window->invoke(workbench_actions::kSaveAll), input::InvokeResult::Ran);
}

TEST_F(WorkbenchTest, CommittedSaveWarningsAreVisibleForExplicitSaveAllAndAutosave) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md"));
    auto* document = app.documents->find("/proj/README.md");
    ASSERT_NE(document, nullptr);
    fs.warnOnSave = true;
    const auto checkWarning = [&] {
        const auto* warning = app.lastNotificationTitled(QStringLiteral("Saved"));
        ASSERT_NE(warning, nullptr);
        EXPECT_EQ(warning->kind, NotificationKind::Warning);
        EXPECT_TRUE(warning->text.contains(QStringLiteral("recovery metadata needs attention")));
        EXPECT_FALSE(document->dirty());
    };
    edit(*document, 0, 0, "manual ");
    ASSERT_TRUE(app.workbench->saveDocument());
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "manual # proj\n");
    checkWarning();
    const int afterManual = app.notificationsTitled(QStringLiteral("Saved"));
    edit(*document, 0, 0, "all ");
    const auto saved = app.workbench->saveAll();
    ASSERT_TRUE(saved.failed.empty());
    ASSERT_EQ(saved.saved.size(), 1U);
    ASSERT_EQ(saved.warnings.size(), 1U);
    EXPECT_EQ(app.notificationsTitled(QStringLiteral("Saved")), afterManual + 1);
    checkWarning();
    edit(*document, 0, 0, "auto ");
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(app.window.get(), &deactivate);
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "auto all manual # proj\n");
    EXPECT_EQ(app.notificationsTitled(QStringLiteral("Saved")), afterManual + 2);
    checkWarning();
}

TEST_F(WorkbenchTest, CloseKeepsWindowOpenToReviewAutosaveWarning) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md"));
    auto* document = app.documents->find("/proj/README.md");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 0, "closing ");
    fs.warnOnSave = true;
    app.window->show();
    EXPECT_FALSE(app.window->close());
    EXPECT_TRUE(app.window->isVisible());
    EXPECT_FALSE(document->dirty());
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "closing # proj\n");
    const auto* warning = app.lastNotificationTitled(QStringLiteral("Saved"));
    ASSERT_NE(warning, nullptr);
    EXPECT_TRUE(warning->text.contains(QStringLiteral("recovery metadata needs attention")));
    const auto* review = app.lastNotificationTitled(QStringLiteral("Review save warnings"));
    ASSERT_NE(review, nullptr);
    EXPECT_TRUE(review->sticky.value_or(false));
    EXPECT_TRUE(app.window->close());
}

TEST_F(WorkbenchTest, CloseWithoutSaveWarningsProceedsNormally) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md"));
    auto* document = app.documents->find("/proj/README.md");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 0, "closing ");
    app.window->show();
    EXPECT_TRUE(app.window->close());
    EXPECT_FALSE(document->dirty());
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "closing # proj\n");
    EXPECT_EQ(app.notificationsTitled(QStringLiteral("Review save warnings")), 0);
}

TEST_F(WorkbenchTest, CloseDoesNotAutosaveWhenFrameAutosaveIsDisabled) {
    Ide& app = ide();
    openProject(app);
    ASSERT_TRUE(app.workbench->openFile("/proj/README.md"));
    auto* document = app.documents->find("/proj/README.md");
    ASSERT_NE(document, nullptr);
    auto policies = app.manager->policies();
    policies.autosave.onFrameDeactivation = false;
    app.manager->setPolicies(policies);
    edit(*document, 0, 0, "unsaved ");
    fs.warnOnSave = true;
    app.window->show();
    EXPECT_TRUE(app.window->close());
    EXPECT_TRUE(document->dirty());
    EXPECT_EQ(memFs.readFile("/proj/README.md").value_or(""), "# proj\n");
    EXPECT_EQ(app.notificationsTitled(QStringLiteral("Saved")), 0);
}

#ifdef _WIN32
TEST(WorkbenchWindows, CaseVariantsOpenOneDocument) {
    frost::test::ScratchDir scratch("workbench-case");
    const std::string file = scratch.join("Foo.cpp");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(file, "int foo;\n").has_value());
    ManualClock clock;
    Ide app(io::realFileSystem(), clock, io::PathStyle::Windows, nullptr);
    ASSERT_TRUE(app.workbench->openFile(file).has_value());
    ASSERT_TRUE(app.workbench->openFile(scratch.join("foo.CPP")).has_value());
    EXPECT_EQ(app.window->tabs().count(), 1);
    EXPECT_EQ(app.manager->documents().size(), 1U);
    EXPECT_TRUE(app.activePath().ends_with("Foo.cpp")) << app.activePath();
}
#endif

} // namespace
} // namespace frost::ui::test
