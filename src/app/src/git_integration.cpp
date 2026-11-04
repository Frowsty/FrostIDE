// SPDX-License-Identifier: Apache-2.0
// Git integration for the actual application workbench.
#include "git_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/document/load_service.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/dialog_chrome.hpp"
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
#include "frost/ui/vcs/vcs_colors.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/vcs/git_locator.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::app {
namespace {
constexpr auto kCommit = "commit";
constexpr auto kDiff = "gitDiff";
constexpr auto kLog = "gitLog";
constexpr auto kConsole = "gitConsole";

// Documents and Git can use different case or symlink spellings for the same file.
std::string fileKey(io::IFileSystem& fs, std::string_view path) {
    return vcs::vcsPathKey(fs.canonicalPath(path).value_or(std::string(path)));
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
    m_discovery = std::jthread([this, fs, launcher, root, configured, generation](std::stop_token stop) {
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
    window.toolWindows().setContent(kCommit, m_commit);
    m_diff = new ui::DiffViewer();
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
    window.mainToolbar().setVcsBranchName(m_branch->text());
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
        if (kind == vcs::GitOperationKind::Rollback || kind == vcs::GitOperationKind::Checkout ||
            kind == vcs::GitOperationKind::Pull)
            m_workbench.checkExternalChanges();
    });
    connect(m_controller.get(), &ui::GitController::statusChanged, this, [this] {
        if (!m_workbench.m_window)
            return;
        if (m_branch)
            m_workbench.m_window->mainToolbar().setVcsBranchName(m_branch->text());
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
    if (kind == vcs::GitOperationKind::Rollback || kind == vcs::GitOperationKind::Checkout ||
        kind == vcs::GitOperationKind::Pull) {
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
    if (kind == vcs::GitOperationKind::Rollback || kind == vcs::GitOperationKind::Checkout ||
        kind == vcs::GitOperationKind::Pull)
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
    const QPointer<ui::DiffViewer> viewer(m_diff);
    const QPointer<GitIntegration> self(this);
    const auto generation = ++m_diffGeneration;
    readBaseText(path, index,
                 [self, viewer, generation, index,
                  working = std::move(working)](Result<std::optional<std::string>> head) mutable {
                     if (!self || self->m_diffGeneration != generation || !viewer || !head)
                         return;
                     auto diff = vcs::SideBySideDiff::compute(head->value_or(std::string()), std::move(working));
                     if (diff)
                         viewer->setDiff(std::move(*diff),
                                         head->has_value() ? (index ? QObject::tr("Index") : QObject::tr("HEAD"))
                                                           : QObject::tr("No base version"),
                                         QObject::tr("Working copy"));
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
} // namespace frost::app
