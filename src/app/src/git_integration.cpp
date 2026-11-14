// SPDX-License-Identifier: Apache-2.0
// Git integration for the actual application workbench.
#include "git_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/diff/merge.hpp"
#include "frost/document/load_service.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/lane.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/text/text_shaper.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/nav/merge_layout.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/vcs/branch_widget.hpp"
#include "frost/ui/vcs/change_markers.hpp"
#include "frost/ui/vcs/commit_window.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/git_actions.hpp"
#include "frost/ui/vcs/git_console.hpp"
#include "frost/ui/vcs/git_controller.hpp"
#include "frost/ui/vcs/git_log_window.hpp"
#include "frost/ui/vcs/merge_guard.hpp"
#include "frost/ui/vcs/merge_editor.hpp"
#include "frost/ui/vcs/vcs_colors.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"
#include "frost/vcs/git_locator.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::app {
namespace {

// Destroys a closed project's Git service (cancels its work and joins its workers) on a pool thread.
sched::Task<void> stopRetiredGitService(std::unique_ptr<vcs::GitService> service) {
    service.reset();
    co_return;
}

constexpr auto kCommit = "commit";
constexpr auto kDiff = "gitDiff";
constexpr auto kLog = "gitLog";
constexpr auto kConsole = "gitConsole";

bool writesWorkingTree(vcs::GitOperationKind kind) {
    switch (kind) {
    case vcs::GitOperationKind::Rollback: case vcs::GitOperationKind::Checkout: case vcs::GitOperationKind::Pull:
    case vcs::GitOperationKind::Merge: case vcs::GitOperationKind::Rebase: case vcs::GitOperationKind::Reset:
    case vcs::GitOperationKind::CherryPick: case vcs::GitOperationKind::Revert:
    case vcs::GitOperationKind::Stash: case vcs::GitOperationKind::Unstash: return true;
    default: return false;
    }
}

// Documents and Git can use different case or symlink spellings for the same file.
std::string fileKey(io::IFileSystem& fs, std::string_view path) {
    return vcs::vcsPathKey(fs.canonicalPath(path).value_or(std::string(path)));
}

// A comparison cannot silently write through a symbolic alias to another file. New conflict
// outputs validate their parent too; the SaveManager remains responsible for atomic encoding/save.
Result<std::string> comparisonTarget(io::IFileSystem& fs, std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized) return std::unexpected(std::move(normalized.error()));
    auto canonical = fs.canonicalPath(path);
    if (!canonical) {
        if (canonical.error().code() != ErrorCode::NotFound) return std::unexpected(std::move(canonical.error()));
        auto parent = fs.canonicalPath(io::parentPath(path));
        if (!parent) return std::unexpected(std::move(parent.error()));
        canonical = io::joinPath(*parent, io::fileName(path));
        if (!canonical) return std::unexpected(std::move(canonical.error()));
    }
    if (vcs::vcsPathKey(*canonical) != vcs::vcsPathKey(*normalized))
        return fail(ErrorCode::InvalidArgument, "Comparison writes require a canonical file path, not a symbolic alias");
    return canonical;
}

// Find a physical worktree marker without launching an untrusted project-selected executable.
std::string containingGitRoot(io::IFileSystem& fs, std::string path) {
    while (!path.empty()) {
        auto marker = io::joinPath(path, ".git");
        if (marker && fs.exists(*marker).value_or(false))
            return path;
        std::string parent(io::parentPath(path));
        if (parent.empty() || parent == path)
            break;
        path = std::move(parent);
    }
    return {};
}
} // namespace

GitIntegration::GitIntegration(Workbench& wb) : QObject(&wb), m_workbench(wb) {
    auto& window = *wb.m_window;
    for (auto [id, title] : {std::pair{kCommit, "Commit"}, std::pair{kDiff, "Diff"}, std::pair{kLog, "Git Log"},
                             std::pair{kConsole, "Git Console"}}) {
        if (window.toolWindows().contains(id))
            continue;
        ui::ToolWindowSpec spec;
        spec.id = id;
        spec.title = QString::fromLatin1(title);
        spec.side = ui::ToolWindowSide::Bottom;
        spec.iconId = std::string(ui::icons::kVcsBranch);
        if (auto added = window.toolWindows().add(std::move(spec)); !added)
            log::warn("git", "tool window: {}", added.error().describe());
    }
    const QPointer<GitIntegration> self(this);
    wb.m_documents->setWriteGuard(
        [self](std::string_view path) -> Result<void> { return self ? self->allowWrite(path) : Result<void>{}; });
    ui::GitActionTargets targets;
    targets.requestTrust = [self] {
        if (self)
            self->requestTrust();
    };
    targets.openCommitWindow = [self] {
        if (self)
            self->m_workbench.m_window->toolWindows().show(kCommit);
    };
    targets.showDiffInCommitWindow = [self] {
        if (self && self->m_commit)
            self->m_commit->showDiff();
    };
    targets.currentFile = [self] {
        auto* view = self ? self->m_workbench.currentView() : nullptr;
        return view ? view->path() : std::string();
    };
    targets.compareWithHead = [self](const std::string& path) {
        if (self)
            self->showDiff(path);
    };
    targets.compareWithIndex = [self](const std::string& path) {
        if (self)
            self->showDiff(path, true);
    };
    targets.showBranches = [self] {
        if (self && self->m_branch)
            self->m_branch->openPopup();
    };
    targets.activeDiffViewer = [self] { return self ? self->m_diff.data() : nullptr; };
    targets.confirm = [self](const QString& title, const QString& text) {
        return self && QMessageBox::question(self->m_workbench.m_window, title, text) == QMessageBox::Yes;
    };
    targets.showLog = [self] {
        if (self) {
            self->m_workbench.m_window->toolWindows().show(kLog);
            if (self->m_log)
                self->m_log->refresh();
        }
    };
    targets.stashChanges = [self] {
        if (self && self->m_branch)
            self->m_branch->promptStashChanges();
    };
    targets.unstashChanges = [self] {
        if (self && self->m_branch)
            self->m_branch->promptUnstashChanges();
    };
    targets.annotate = [self](const std::string& path) {
        if (self)
            self->showBlame(path);
    };
    targets.resolveConflicts = [self] {
        if (self)
            self->showResolveConflictsDialog();
    };
    if (auto registered = ui::registerGitActions(
            wb.m_registry, [self] { return self ? self->controller() : nullptr; }, std::move(targets));
        !registered)
        log::warn("git", "actions: {}", registered.error().describe());
    m_diffRefresh.setSingleShot(true);
    m_diffRefresh.setInterval(300);
    connect(&m_diffRefresh, &QTimer::timeout, this, [this] {
        if (!m_diffPath.empty())
            showDiff(m_diffPath, m_diffIndex);
    });
    m_poll.setInterval(500);
    connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
    m_poll.start();
}

GitIntegration::~GitIntegration() { shutdown(); }

void GitIntegration::openProject(const std::string& root) {
    closeProject();
    if (m_shutdown || root.empty())
        return;
    m_root = root;
    const auto generation = ++m_projectGeneration;
    if (m_workbench.m_window)
        m_workbench.m_window->setProjectTrusted(m_workbench.m_services.trust.isTrusted(root));
    const auto configured = vcs::configuredGitPath(*m_workbench.m_services.settings.snapshot());
    m_configuredGit = configured;
    auto* fs = &m_workbench.m_services.fs;
    auto* launcher = &m_workbench.m_services.launcher;
    m_discovery = std::jthread([this, fs, launcher, root, configured, generation](const std::stop_token& stop) {
        const std::string gitRoot = containingGitRoot(*fs, root);
        if (gitRoot.empty() || stop.stop_requested())
            return;
        auto search = vcs::defaultGitSearch(configured);
        search.excludedDirectories.push_back(root);
        search.excludedDirectories.push_back(gitRoot);
        auto report = [this, root, generation](const Error& error) {
            const QString message = QString::fromStdString(error.message());
            QMetaObject::invokeMethod(
                this,
                [this, root, generation, message] {
                    if (!m_shutdown && m_projectGeneration == generation && m_root == root && m_workbench.m_window)
                        m_workbench.m_window->notifications().notify(ui::Notification{
                            tr("Git unavailable"), message, ui::NotificationKind::Warning, std::nullopt, {}});
                },
                Qt::QueuedConnection);
        };
        auto found = vcs::findGit(*fs, search);
        if (!found) {
            if (!stop.stop_requested())
                report(found.error());
            return;
        }
        if (stop.stop_requested())
            return;
        vcs::GitRunnerOptions options;
        options.trusted = false;
        vcs::GitRunner runner(*launcher, found->path, options);
        auto repository = vcs::discoverRepository(runner, root, stop);
        if (!repository) {
            if (!stop.stop_requested())
                report(repository.error());
            return;
        }
        if (!*repository || stop.stop_requested())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, info = std::move(**repository), executable = found->path, root, generation] {
                if (!m_shutdown && m_projectGeneration == generation && m_root == root)
                    installRepository(info, executable);
            },
            Qt::QueuedConnection);
    });
}

void GitIntegration::closeProject() {
    ++m_projectGeneration;
    m_root.clear();
    if (m_trustDialog) {
        m_trustDialog->close();
        m_trustDialog = nullptr;
    }
    if (m_workbench.m_window)
        m_workbench.m_window->setProjectTrusted(false);
    if (m_discovery.joinable()) {
        m_discovery.request_stop();
        m_discovery.join();
    }
    clearRepository();
}
void GitIntegration::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_poll.stop();
    closeProject();
    m_workbench.m_documents->setWriteGuard({});
}
void GitIntegration::clearRepository() {
    auto* window = m_workbench.m_window.data();
    if (window) {
        window->mainToolbar().setVcsBranchName({});
        window->mainToolbar().setVcsMenuBuilder({});
        window->tabs().setTitleColorProvider({});
        if (m_workbench.m_nav && m_workbench.m_nav->fileSystemView())
            m_workbench.m_nav->fileSystemView()->setFileColorProvider({});
        if (m_workbench.m_explorer)
            m_workbench.m_explorer->model()->setFileColorProvider({});
        for (const char* id : {kCommit, kDiff, kLog, kConsole})
            window->toolWindows().setContent(id, new QLabel(tr("No Git repository is open.")));
    }
    for (auto* view : m_workbench.m_editors->views()) {
        const auto markers =
            view->editor().findChildren<ui::EditorChangeMarkers*>(QString(), Qt::FindDirectChildrenOnly);
        for (auto* marker : markers)
            delete marker;
        view->editor().setLineChanges({});
    }
    if (m_branch)
        delete m_branch.data();
    m_commit = nullptr;
    m_diff = nullptr;
    m_log = nullptr;
    m_diffRefresh.stop();
    m_diffPath.clear();
    m_dirtyPaths.clear();
    m_dirtyKeys.clear();
    m_controller.reset();
    m_destructiveOperation = false;
}

void GitIntegration::installRepository(vcs::RepositoryInfo info, const std::string& executable) {
    const QString draft = m_commit ? m_commit->message() : QString();
    const bool amend = m_commit && m_commit->amendCheck()->isChecked();
    std::vector<std::pair<std::string, bool>> choices;
    if (m_commit) {
        for (const auto& path : m_commit->model().allPaths())
            choices.emplace_back(path, m_commit->model().isChecked(path));
    }
    clearRepository();
    m_executable = executable;
    m_trusted = m_workbench.m_services.trust.isTrusted(m_root) && m_workbench.m_services.trust.isTrusted(info.workTree);
    vcs::GitRunnerOptions options;
    options.trusted = m_trusted;
    auto* trust = &m_workbench.m_services.trust;
    const std::string project = m_root;
    const std::string workTree = info.workTree;
    options.stillTrusted = [trust, project, workTree] {
        return trust->isTrusted(project) && trust->isTrusted(workTree);
    };
    m_controller =
        std::make_unique<ui::GitController>(vcs::GitRunner(m_workbench.m_services.launcher, executable, options),
                                            m_workbench.m_services.fs, std::move(info), vcs::GitServiceOptions{}, this);
    // A closed project's service is joined on the pool, not on the GUI thread: its git process may be
    // starting or ending (FND-9b; a 600 ms freeze in R3's j2_run). The launcher and the file system it
    // uses are the application's and outlive the Workbench, which joins m_retiredGit before its pool goes.
    m_controller->setServiceRetirer([workbench = &m_workbench](std::unique_ptr<vcs::GitService> service) {
        if (!workbench->m_pool) {
            service.reset();
            return;
        }
        std::erase_if(workbench->m_retiredGit, [](auto& stopped) {
            if (!stopped.ready())
                return false;
            stopped.join();
            return true;
        });
        workbench->m_retiredGit.push_back(
            workbench->m_pool->spawn(sched::Lane::Background, stopRetiredGitService(std::move(service))));
    });
    auto& window = *m_workbench.m_window;
    window.setProjectTrusted(m_trusted);
    m_controller->setNotifications(&window.notifications());
    m_controller->setTaskManager(&window.backgroundTasks());
    m_controller->setOperationGate([this](auto kind, const auto& paths) { return allow(kind, paths); });
    m_commit = new ui::CommitToolWindow(*m_controller);
    m_commit->setMessage(draft);
    m_commit->amendCheck()->setChecked(amend);
    if (!choices.empty()) {
        auto saved = std::make_shared<std::vector<std::pair<std::string, bool>>>(std::move(choices));
        connect(m_controller.get(), &ui::GitController::statusChanged, m_commit, [this, saved] {
            if (!m_commit || saved->empty())
                return;
            for (const auto& [path, checked] : *saved)
                m_commit->model().setChecked(path, checked);
            saved->clear();
        });
    }
    connect(m_commit, &ui::CommitToolWindow::diffRequested, this,
            [this](const QString& path) { showDiff(path.toUtf8().toStdString()); });
    connect(m_commit, &ui::CommitToolWindow::resolveConflictsRequested, this,
            &GitIntegration::showResolveConflictsDialog);
    window.toolWindows().setContent(kCommit, m_commit);
    m_diff = new ui::DiffViewer(nullptr, {.languages = &m_workbench.m_languages,
        .scheduler = m_workbench.m_pool.get(), .clock = &m_workbench.m_services.clock,
        .actionHost = &m_workbench.m_actionHost});
    window.toolWindows().setContent(kDiff, m_diff);
    m_log = new ui::GitLogToolWindow(m_controller.get());
    connect(m_log, &ui::GitLogToolWindow::openCommitFileRequested, this,
            [this](const QString&, const QString& relPath) {
                if (!m_controller)
                    return;
                if (auto joined = io::joinPath(m_controller->info().workTree, relPath.toUtf8().toStdString()))
                    showDiff(*joined);
            });
    window.toolWindows().setContent(kLog, m_log);
    window.toolWindows().setContent(kConsole, new ui::GitConsoleView(*m_controller));
    m_branch = new ui::BranchStatusWidget(*m_controller, window.statusBar());
    window.statusBar()->addPermanentWidget(m_branch);
    const QPointer<GitIntegration> self(this);
    m_controller->readBranches();
    window.mainToolbar().setVcsBranchName(m_branch->branchName());
    window.mainToolbar().setVcsMenuBuilder([self](QMenu& menu) {
        if (self && self->m_branch) {
            if (self->m_controller) {
                self->m_controller->readBranches();
            }
            self->m_branch->populateMenu(menu);
        }
    });
    auto color = [self](std::string_view path) -> std::optional<QColor> {
        if (!self || !self->m_controller)
            return std::nullopt;
        const auto status = self->m_controller->statuses().statusOf(path);
        if (!status)
            return std::nullopt;
        if (*status == vcs::VcsFileStatus::Unmodified) {
            if (self->m_dirtyKeys.contains(vcs::vcsPathKey(path)))
                return ui::vcsStatusColor(vcs::VcsFileStatus::Modified);
        }
        return ui::vcsStatusColor(*status);
    };
    window.tabs().setTitleColorProvider(color);
    if (m_workbench.m_nav && m_workbench.m_nav->fileSystemView())
        m_workbench.m_nav->fileSystemView()->setFileColorProvider(color);
    if (m_workbench.m_explorer)
        m_workbench.m_explorer->model()->setFileColorProvider(color);
    connect(m_controller.get(), &ui::GitController::operationFinished, this, [this](int rawKind, bool, const QString&) {
        const auto kind = static_cast<vcs::GitOperationKind>(rawKind);
        m_destructiveOperation = false;
        if (writesWorkingTree(kind))
            m_workbench.checkExternalChanges();
    });
    connect(m_controller.get(), &ui::GitController::statusChanged, this, [this] {
        if (!m_workbench.m_window)
            return;
        if (m_branch)
            m_workbench.m_window->mainToolbar().setVcsBranchName(m_branch->branchName());
        m_workbench.m_window->tabs().refreshTitleColors();
        if (m_workbench.m_nav && m_workbench.m_nav->fileSystemView())
            m_workbench.m_nav->fileSystemView()->refreshFileColors();
        if (m_workbench.m_explorer)
            m_workbench.m_explorer->model()->refreshFileColors();
        refreshMarkers();
    });
    for (auto* view : m_workbench.m_editors->views())
        attachEditor(*view);
    documentChanged();
    window.refreshActions();
}

void GitIntegration::editorChanged() {
    if (m_destructiveOperation && m_controller)
        m_controller->cancelOperation();
    documentChanged();
}

void GitIntegration::documentChanged() {
    if (!m_commit || !m_controller)
        return;
    if (!m_diffPath.empty() && m_workbench.m_window && m_workbench.m_window->toolWindows().isVisible(kDiff))
        m_diffRefresh.start();
    std::vector<std::string> paths;
    for (const auto& doc : m_workbench.m_documents->documents()) {
        if (!doc->untitled() && doc->dirty() && m_controller->service().repository().relativePath(doc->path()))
            paths.push_back(doc->path());
    }
    if (m_dirtyPaths == paths)
        return;
    m_dirtyPaths = paths;
    m_dirtyKeys.clear();
    for (const auto& path : paths)
        m_dirtyKeys.insert(vcs::vcsPathKey(path));
    m_commit->setUnsavedPaths(std::move(paths));
    if (m_workbench.m_window)
        m_workbench.m_window->tabs().refreshTitleColors();
    if (m_workbench.m_nav && m_workbench.m_nav->fileSystemView())
        m_workbench.m_nav->fileSystemView()->refreshFileColors();
    if (m_workbench.m_explorer)
        m_workbench.m_explorer->model()->refreshFileColors();
}

void GitIntegration::attachEditor(DocumentEditorView& view) {
    if (!m_controller || !m_controller->service().repository().relativePath(view.path()))
        return;
    if (!view.editor().findChildren<ui::EditorChangeMarkers*>(QString(), Qt::FindDirectChildrenOnly).empty())
        return;
    const QPointer<GitIntegration> self(this);
    new ui::EditorChangeMarkers(view.editor(), [self](const std::string& path, auto done) {
        if (self)
            self->readBaseText(path, false, std::move(done));
    });
}
void GitIntegration::refreshMarkers() {
    for (auto* view : m_workbench.m_editors->views()) {
        for (auto* marker :
             view->editor().findChildren<ui::EditorChangeMarkers*>(QString(), Qt::FindDirectChildrenOnly))
            marker->refreshBase();
    }
}
void GitIntegration::filesChanged(const io::FileChangeBatch& batch) {
    if (m_controller)
        m_controller->service().applyChanges(batch);
}
void GitIntegration::poll() {
    if (m_shutdown || m_root.empty())
        return;
    if (vcs::configuredGitPath(*m_workbench.m_services.settings.snapshot()) != m_configuredGit) {
        const std::string root = m_root;
        openProject(root);
        return;
    }
    if (!m_controller) {
        if (m_workbench.m_window)
            m_workbench.m_window->setProjectTrusted(m_workbench.m_services.trust.isTrusted(m_root));
        return;
    }
    const bool trusted = m_workbench.m_services.trust.isTrusted(m_root) &&
                         m_workbench.m_services.trust.isTrusted(m_controller->info().workTree);
    if (trusted != m_trusted) {
        auto info = m_controller->info();
        const auto executable = m_executable;
        installRepository(std::move(info), executable);
        return;
    }
    // Project watch may not cover the enclosing worktree or linked-worktree git directory.
    if (++m_pollCount % 3 == 0)
        m_controller->refreshStatus();
}

void GitIntegration::requestTrust() {
    if (!m_controller || m_shutdown || m_trustDialog)
        return;
    if (m_workbench.m_services.trust.isTrusted(m_controller->info().workTree))
        return;
    if (m_workbench.m_projectTrustDialog) {
        delete m_workbench.m_projectTrustDialog.data();
        m_workbench.m_projectTrustDialog.clear();
    }
    auto* dialog = new ui::TrustDialog(*m_workbench.m_build, {QString::fromStdString(m_controller->info().workTree)},
                                       m_workbench.m_window, ui::TrustPurpose::GitRepository);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_trustDialog = dialog;
    dialog->open();
}

Result<void> GitIntegration::allow(vcs::GitOperationKind kind, const std::vector<std::string>& paths) {
    if (m_shutdown || !m_controller || !m_workbench.m_services.trust.isTrusted(m_root) ||
        !m_workbench.m_services.trust.isTrusted(m_controller->info().workTree))
        return fail(ErrorCode::Untrusted, "Trust the repository before running Git operations");
    if (kind == vcs::GitOperationKind::Commit) {
        for (const auto& path : paths) {
            const auto key = fileKey(m_workbench.m_services.fs, path);
            for (const auto& doc : m_workbench.m_documents->documents()) {
                if (doc->untitled() || !doc->dirty() || fileKey(m_workbench.m_services.fs, doc->path()) != key)
                    continue;
                if (auto saved = m_workbench.saveQuietly(*doc); !saved)
                    return std::unexpected(std::move(saved.error()));
            }
        }
    }
    if (writesWorkingTree(kind)) {
        for (const auto& doc : m_workbench.m_documents->documents()) {
            if (!doc->dirty())
                continue;
            const auto key = fileKey(m_workbench.m_services.fs, doc->path());
            const bool selected = std::ranges::any_of(
                paths, [&](const auto& path) { return fileKey(m_workbench.m_services.fs, path) == key; });
            const auto canonical = m_workbench.m_services.fs.canonicalPath(doc->path()).value_or(doc->path());
            if (!selected && !m_controller->service().repository().relativePath(doc->path()) &&
                !m_controller->service().repository().relativePath(canonical))
                continue;
            if (kind != vcs::GitOperationKind::Rollback || selected)
                return fail(ErrorCode::Busy, "Save or close unsaved files before this Git operation");
        }
    }
    if (writesWorkingTree(kind))
        m_destructiveOperation = true;
    return {};
}

Result<void> GitIntegration::allowWrite(std::string_view path) const {
    if (!m_destructiveOperation || !m_controller)
        return {};
    const auto canonical = m_workbench.m_services.fs.canonicalPath(path).value_or(std::string(path));
    const auto& repository = m_controller->service().repository();
    if (repository.relativePath(path) || repository.relativePath(canonical))
        return fail(ErrorCode::Busy, "Wait for the Git operation to finish before saving this file");
    return {};
}

void GitIntegration::readBaseText(const std::string& path, bool index, ContentDone done) {
    if (!m_controller)
        return;
    const QPointer<GitIntegration> self(this);
    auto finished = [self, path, done = std::move(done)](Result<std::optional<std::string>> bytes) mutable {
        if (!self)
            return;
        if (!bytes) {
            done(std::unexpected(std::move(bytes.error())));
            return;
        }
        if (!*bytes) {
            done(std::optional<std::string>{});
            return;
        }
        document::RawFile raw;
        raw.bytes = std::move(**bytes);
        auto decoded = self->m_workbench.m_documents->loadService().decode(std::move(raw), path);
        if (!decoded) {
            done(std::unexpected(std::move(decoded.error())));
            return;
        }
        done(std::optional<std::string>(decoded->buffer.snapshot().text()));
    };
    if (index)
        m_controller->readIndexContent(path, std::move(finished));
    else
        m_controller->readHeadContent(path, std::move(finished));
}

void GitIntegration::showDiff(const std::string& path, bool index) {
    if (!m_controller || !m_diff)
        return;
    if (m_diff->hasWorkingEdits()) {
        if (path != m_diffPath) m_workbench.notify(tr("Diff"), tr("Apply or undo the current comparison edits before switching files."), ui::NotificationKind::Warning);
        return;
    }
    m_diffPath = path;
    m_diffIndex = index;
    std::string working;
    document::FileDocument* open = nullptr;
    const auto key = fileKey(m_workbench.m_services.fs, path);
    for (const auto& doc : m_workbench.m_documents->documents()) {
        if (!doc->untitled() && fileKey(m_workbench.m_services.fs, doc->path()) == key) {
            open = doc.get();
            break;
        }
    }
    if (open) {
        working = open->document().committedText().text();
    } else {
        auto loaded = m_workbench.m_documents->loadService().load(path);
        if (!loaded && loaded.error().code() != ErrorCode::NotFound)
            return;
        if (loaded)
            working = loaded->buffer.snapshot().text();
    }
    const auto expectedDisk = m_workbench.m_services.fs.readFile(path);
    if (!expectedDisk && expectedDisk.error().code() != ErrorCode::NotFound) return;
    std::optional<storage::DiskBase> diskReceipt;
    if (expectedDisk) diskReceipt = storage::DiskBase{.size = expectedDisk->size(), .fingerprint = storage::xxh3Hash128(*expectedDisk)};
    const auto projectGeneration = m_projectGeneration;
    const QPointer<ui::DiffViewer> viewer(m_diff);
    const QPointer<GitIntegration> self(this);
    const auto generation = ++m_diffGeneration;
    readBaseText(path, index,
                 [self, viewer, generation, index, path, diskReceipt, projectGeneration,
                  working = std::move(working)](Result<std::optional<std::string>> head) mutable {
                     if (!self || self->m_diffGeneration != generation || !viewer || !head || viewer->hasWorkingEdits())
                         return;
                     auto diff = vcs::SideBySideDiff::compute(head->value_or(std::string()), std::move(working));
                     if (diff) {
                         viewer->setApplyHandler({});
                         viewer->setDiff(std::move(*diff),
                                         head->has_value() ? (index ? QObject::tr("Index") : QObject::tr("HEAD"))
                                                           : QObject::tr("No base version"),
                                         QObject::tr("Working copy"));
                         viewer->leftPane()->setPath(path);
                         viewer->rightPane()->setPath(path);
                         viewer->setApplyHandler([self, path, diskReceipt, projectGeneration](std::string_view expected, std::string_view result) mutable -> Result<void> {
                             if (!self || self->m_projectGeneration != projectGeneration)
                                 return fail(ErrorCode::Cancelled, "The comparison's project changed");
                             if (!self->m_controller || self->m_controller->busy() || self->m_shutdown ||
                                 !self->m_workbench.m_services.trust.isTrusted(self->m_root) ||
                                 !self->m_workbench.m_services.trust.isTrusted(self->m_controller->info().workTree) ||
                                 !self->m_workbench.m_services.trust.isTrusted(path))
                                 return fail(ErrorCode::Untrusted, "Apply requires an idle trusted repository");
                             if (auto relative = self->m_controller->service().repository().relativePath(path); !relative)
                                 return std::unexpected(std::move(relative.error()));
                             if (auto allowed = self->allowWrite(path); !allowed) return allowed;
                             auto& fs = self->m_workbench.m_services.fs;
                             auto target = comparisonTarget(fs, path);
                             if (!target) return std::unexpected(std::move(target.error()));
                             if (!self->m_workbench.m_services.trust.isTrusted(*target))
                                 return fail(ErrorCode::Untrusted, "The comparison target is not trusted");
                             auto disk = fs.readFile(path);
                             if ((!disk && disk.error().code() != ErrorCode::NotFound) ||
                                 static_cast<bool>(disk) != diskReceipt.has_value() ||
                                 (disk && (disk->size() != diskReceipt->size || storage::xxh3Hash128(*disk) != diskReceipt->fingerprint)))
                                 return fail(ErrorCode::Busy, "The file changed on disk; reopen the comparison before applying");
                             auto opened = self->m_workbench.m_documents->open(path);
                             if (!opened) return std::unexpected(std::move(opened.error()));
                             auto& document = **opened;
                             if (document.document().committedText().text() != expected)
                                 return fail(ErrorCode::Busy, "The open document changed; reopen the comparison before applying");
                             auto change = document.document().replace({ByteOffset{0}, document.document().text().size()}, result, {});
                             if (!change) return std::unexpected(std::move(change.error()));
                             auto saved = self->m_workbench.m_documents->save(document);
                             if (!saved) {
                                 static_cast<void>(document.document().undo());
                                 if (auto* view = self->m_workbench.m_editors->find(document)) view->reloadFromDocument();
                                 return std::unexpected(std::move(saved.error()));
                             }
                             // SaveManager's receipt describes the exact encoded bytes it wrote; never
                             // adopt a later independent disk read as the baseline for another Apply.
                             diskReceipt = document.disk();
                             if (auto* view = self->m_workbench.m_editors->find(document)) { view->reloadFromDocument(); view->markSaved(); }
                             self->documentChanged();
                             return {};
                         });
                     }
                 });
    m_workbench.m_window->toolWindows().show(kDiff);
}

void GitIntegration::showBlame(const std::string& path) {
    if (!m_controller || path.empty())
        return;
    const QPointer<GitIntegration> self(this);
    m_controller->readBlame(
        QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())),
        [self, path](Result<std::vector<vcs::GitBlameLine>> result) {
            if (!self || !self->m_workbench.m_window || !result)
                return;
            QDialog dialog(self->m_workbench.m_window);
            dialog.setObjectName(QStringLiteral("gitBlameDialog"));
            dialog.setWindowTitle(QObject::tr("Git Annotate — %1")
                                      .arg(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()))));
            dialog.resize(840, 560);
            auto* layout = new QVBoxLayout(&dialog);
            auto* tree = new QTreeWidget(&dialog);
            tree->setRootIsDecorated(false);
            tree->setColumnCount(5);
            tree->setHeaderLabels({QObject::tr("Line"), QObject::tr("Commit"), QObject::tr("Author"),
                                   QObject::tr("Date"), QObject::tr("Summary")});
            tree->header()->setStretchLastSection(true);
            for (const auto& entry : *result) {
                auto* item = new QTreeWidgetItem(tree);
                item->setText(0, QString::number(entry.line));
                item->setText(1, QString::fromStdString(entry.shortHash));
                item->setText(2, QString::fromStdString(entry.author));
                item->setText(3, QString::fromStdString(entry.dateShort));
                item->setText(4, QString::fromStdString(entry.summary));
            }
            layout->addWidget(tree, 1);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            ui::installDialogChrome(&dialog);
            dialog.exec();
        });
}

void GitIntegration::showResolveConflictsDialog() {
    if (!m_controller || !m_workbench.m_window)
        return;

    struct ConflictSides {
        std::string base, ours, theirs;
        bool oursExists = false, theirsExists = false, valid = false;
        std::optional<std::string> disk;
        std::string index, stages;
        std::uint64_t generation = 0;
    };
    const auto report = [this](std::string_view message) {
        m_workbench.notify(tr("Resolve Conflicts"), QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())), ui::NotificationKind::Error);
    };
    const auto readStages = [this](const std::string& relative) -> Result<std::string> {
        if (!m_controller || m_controller->busy()) return fail(ErrorCode::Busy, "Wait for the Git operation to finish");
        vcs::GitInvocation invocation;
        invocation.arguments = {"ls-files", "--unmerged", "-z", "--", relative};
        invocation.workingDirectory = m_controller->info().workTree;
        invocation.background = true;
        auto output = m_controller->service().repository().runner().run(invocation);
        if (!output) return std::unexpected(std::move(output.error()));
        if (!output->succeeded()) return std::unexpected(vcs::gitFailure("Read conflict stages", *output));
        return std::move(output->stdoutBytes);
    };
    const auto extractSides = [&](const std::string& absolute, const std::string& relative) -> ConflictSides {
        ConflictSides sides;
        sides.generation = m_projectGeneration;
        auto stages = readStages(relative);
        if (!stages || stages->empty()) { report(stages ? "The file no longer has unmerged Git stages" : stages.error().message()); return sides; }
        sides.stages = *stages;
        auto indexPath = io::joinPath(m_controller->info().gitDir, "index");
        if (!indexPath) { report(indexPath.error().message()); return sides; }
        auto index = m_workbench.m_services.fs.readFile(*indexPath);
        auto disk = m_workbench.m_services.fs.readFile(absolute);
        if (!index || (!disk && disk.error().code() != ErrorCode::NotFound)) {
            report(!index ? index.error().message() : disk.error().message()); return sides;
        }
        sides.index = std::move(*index);
        if (disk) sides.disk = std::move(*disk);
        // Git's actual stage blobs are authoritative. Conflict markers omit the common ancestor
        // in normal merge style and are never used to invent a base version.
        for (int stage = 1; stage <= 3; ++stage) {
            const std::string marker = " " + std::to_string(stage) + "\t";
            if (sides.stages.find(marker) == std::string::npos) continue;
            vcs::GitInvocation invocation;
            invocation.arguments = {"cat-file", "blob", ":" + std::to_string(stage) + ":" + relative};
            invocation.workingDirectory = m_controller->info().workTree;
            invocation.background = true;
            auto output = m_controller->service().repository().runner().run(invocation);
            if (!output || !output->succeeded()) { report(output ? "Could not read the conflict blob" : output.error().message()); return sides; }
            document::RawFile raw;
            raw.bytes = std::move(output->stdoutBytes);
            auto decoded = m_workbench.m_documents->loadService().decode(std::move(raw), absolute);
            if (!decoded) { report(decoded.error().message()); return sides; }
            if (stage == 1) sides.base = decoded->buffer.snapshot().text();
            if (stage == 2) { sides.ours = decoded->buffer.snapshot().text(); sides.oursExists = true; }
            if (stage == 3) { sides.theirs = decoded->buffer.snapshot().text(); sides.theirsExists = true; }
        }
        auto stillStages = readStages(relative);
        auto stillIndex = m_workbench.m_services.fs.readFile(*indexPath);
        if (!stillStages || !stillIndex || *stillStages != sides.stages || *stillIndex != sides.index) {
            report("Git conflict sources changed while opening the merge; open it again"); return sides;
        }
        sides.valid = true;
        return sides;
    };
    const auto applyResolved = [&](const std::string& absolute, const std::string& relative,
        const std::string& resolved, const ConflictSides& source, bool deleted = false) -> bool {
        if (!source.valid || source.generation != m_projectGeneration || m_shutdown || !m_controller || m_controller->busy() ||
            !m_workbench.m_services.trust.isTrusted(m_root) || !m_workbench.m_services.trust.isTrusted(m_controller->info().workTree) ||
            !m_workbench.m_services.trust.isTrusted(absolute)) { report("Resolve requires the same idle trusted repository"); return false; }
        if (auto allowed = allowWrite(absolute); !allowed) { report(allowed.error().message()); return false; }
        auto target = comparisonTarget(m_workbench.m_services.fs, absolute);
        if (!target) { report(target.error().message()); return false; }
        if (!m_workbench.m_services.trust.isTrusted(*target)) { report("The merge output target is not trusted"); return false; }
        const auto& repository = m_controller->service().repository();
        if (auto relativePath = repository.relativePath(absolute); !relativePath || *relativePath != relative) {
            report("Conflict path no longer belongs to this repository"); return false;
        }
        m_workbench.checkExternalChanges();
        for (const auto& document : m_workbench.m_documents->documents())
            if (!document->untitled() && document->dirty() && fileKey(m_workbench.m_services.fs, document->path()) == fileKey(m_workbench.m_services.fs, absolute)) {
                report("Save or close the unsaved editor before resolving this conflict"); return false;
            }
        auto stages = readStages(relative);
        auto indexPath = io::joinPath(m_controller->info().gitDir, "index");
        auto index = indexPath ? m_workbench.m_services.fs.readFile(*indexPath) : Result<std::string>(fail(ErrorCode::InvalidArgument, "Invalid index path"));
        auto disk = m_workbench.m_services.fs.readFile(absolute);
        if (!stages || !index || *stages != source.stages || *index != source.index ||
            (!disk && disk.error().code() != ErrorCode::NotFound) || (disk ? std::optional<std::string>(*disk) : std::nullopt) != source.disk) {
            report("The file or Git index changed while merging; reopen the conflict before applying"); return false;
        }
        if (deleted) {
            if (disk) if (auto removed = m_workbench.m_services.fs.removeFile(absolute); !removed) { report(removed.error().message()); return false; }
        } else {
            document::FileDocument* document = nullptr;
            if (disk) {
                auto opened = m_workbench.m_documents->open(absolute);
                if (!opened) { report(opened.error().message()); return false; }
                document = *opened;
            } else document = &m_workbench.m_documents->createUntitled({});
            auto changed = document->document().replace({ByteOffset{0}, document->document().text().size()}, resolved, {});
            if (!changed) { report(changed.error().message()); return false; }
            auto saved = disk ? m_workbench.m_documents->save(*document) : m_workbench.m_documents->saveAs(*document, absolute);
            if (!saved) {
                static_cast<void>(document->document().undo());
                if (auto* view = m_workbench.m_editors->find(*document)) view->reloadFromDocument();
                report(saved.error().message()); return false;
            }
            if (auto* view = m_workbench.m_editors->find(*document)) { view->reloadFromDocument(); view->markSaved(); }
        }
        vcs::GitInvocation stage;
        stage.arguments = {"add", "-A", "--", relative};
        stage.workingDirectory = m_controller->info().workTree;
        stage.requiresTrust = true;
        auto staged = repository.runner().run(stage);
        if (!staged || !staged->succeeded()) {
            report(staged ? vcs::gitFailure("Stage resolved file", *staged).message() : staged.error().message());
            // The chosen file is retained; never claim that a failed git add resolved the index.
            m_controller->refreshStatus(); return false;
        }
        m_workbench.checkExternalChanges();
        m_controller->refreshStatus();
        documentChanged();
        return true;
    };

    QDialog dialog(m_workbench.m_window);
    dialog.setObjectName(QStringLiteral("gitResolveConflictsDialog"));
    dialog.setWindowTitle(tr("Resolve Conflicts"));
    dialog.resize(760, 480);

    auto* mainLayout = new QVBoxLayout(&dialog);
    auto* headerLabel = new QLabel(tr("Files with merge or rebase conflicts:"), &dialog);
    mainLayout->addWidget(headerLabel);

    auto* bodyLayout = new QHBoxLayout();
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("conflictedFilesList"));
    bodyLayout->addWidget(list, 1);

    auto* actionColumn = new QVBoxLayout();
    auto* acceptYoursBtn = new QPushButton(tr("Accept Yours"), &dialog);
    acceptYoursBtn->setObjectName(QStringLiteral("acceptYoursButton"));
    auto* acceptTheirsBtn = new QPushButton(tr("Accept Theirs"), &dialog);
    acceptTheirsBtn->setObjectName(QStringLiteral("acceptTheirsButton"));
    auto* mergeBtn = new QPushButton(tr("Merge..."), &dialog);
    mergeBtn->setObjectName(QStringLiteral("mergeConflictsButton"));
    actionColumn->addWidget(acceptYoursBtn);
    actionColumn->addWidget(acceptTheirsBtn);
    actionColumn->addWidget(mergeBtn);
    actionColumn->addStretch(1);
    bodyLayout->addLayout(actionColumn);
    mainLayout->addLayout(bodyLayout, 1);

    auto* emptyLabel = new QLabel(tr("No conflicted files in the working tree."), &dialog);
    emptyLabel->setObjectName(QStringLiteral("noConflictsLabel"));
    mainLayout->addWidget(emptyLabel);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
    ui::installDialogChrome(&dialog, true);

    if (m_controller->snapshot()) {
        const std::string& workTree = m_controller->info().workTree;
        for (const vcs::GitStatusEntry& entry : m_controller->snapshot()->status.entries) {
            if (entry.fileStatus() != vcs::VcsFileStatus::Conflicted)
                continue;
            const std::string absPath = vcs::absoluteGitPath(workTree, entry.path);
            auto* item = new QListWidgetItem(
                QString::fromUtf8(entry.path.data(), static_cast<qsizetype>(entry.path.size())), list);
            item->setData(Qt::UserRole, QString::fromStdString(absPath));
            item->setData(Qt::UserRole + 1, QString::fromStdString(entry.path));
        }
    }
    if (list->count() > 0)
        list->setCurrentRow(0);

    const auto updateDialogState = [list, acceptYoursBtn, acceptTheirsBtn, mergeBtn, emptyLabel] {
        const bool hasSelection = list->currentItem() != nullptr;
        acceptYoursBtn->setEnabled(hasSelection);
        acceptTheirsBtn->setEnabled(hasSelection);
        mergeBtn->setEnabled(hasSelection);
        emptyLabel->setVisible(list->count() == 0);
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, [updateDialogState](int) { updateDialogState(); });
    updateDialogState();

    connect(acceptYoursBtn, &QPushButton::clicked, &dialog, [&] {
        QListWidgetItem* item = list->currentItem();
        if (item == nullptr)
            return;
        const std::string absPath = item->data(Qt::UserRole).toString().toStdString();
        const std::string relPath = item->data(Qt::UserRole + 1).toString().toStdString();
        const ConflictSides sides = extractSides(absPath, relPath);
        if (applyResolved(absPath, relPath, sides.ours, sides, !sides.oursExists)) {
            delete list->takeItem(list->row(item));
            updateDialogState();
        }
    });

    connect(acceptTheirsBtn, &QPushButton::clicked, &dialog, [&] {
        QListWidgetItem* item = list->currentItem();
        if (item == nullptr)
            return;
        const std::string absPath = item->data(Qt::UserRole).toString().toStdString();
        const std::string relPath = item->data(Qt::UserRole + 1).toString().toStdString();
        const ConflictSides sides = extractSides(absPath, relPath);
        if (applyResolved(absPath, relPath, sides.theirs, sides, !sides.theirsExists)) {
            delete list->takeItem(list->row(item));
            updateDialogState();
        }
    });

    const auto openThreeWayMerge = [&](QListWidgetItem* item) {
        if (item == nullptr)
            return;
        const std::string absPath = item->data(Qt::UserRole).toString().toStdString();
        const std::string relPath = item->data(Qt::UserRole + 1).toString().toStdString();
        const ConflictSides sides = extractSides(absPath, relPath);
        if (!sides.valid) return;

        ui::EditorServices services{.languages = &m_workbench.m_languages, .scheduler = m_workbench.m_pool.get(),
            .clock = &m_workbench.m_services.clock, .actionHost = &m_workbench.m_actionHost};
        auto merged = ui::showMergeEditor({.base = sides.base, .ours = sides.ours, .theirs = sides.theirs, .path = absPath},
            [&](std::string_view resolved) -> Result<void> {
                if (applyResolved(absPath, relPath, std::string(resolved), sides)) return {};
                return fail(ErrorCode::Busy, "The merge could not be applied; source changes or write/stage errors are shown in notifications");
            }, services, &dialog);
        if (!merged) { report(merged.error().message()); return; }
        if (*merged) {
            delete list->takeItem(list->row(item));
            updateDialogState();
        }
    };

    connect(mergeBtn, &QPushButton::clicked, &dialog, [&] { openThreeWayMerge(list->currentItem()); });
    connect(list, &QListWidget::itemDoubleClicked, &dialog, openThreeWayMerge);

    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}
} // namespace frost::app
