// SPDX-License-Identifier: Apache-2.0
//
// Workbench action registration, File System and TODO tool windows, shortcut display, and TODO
// scanning.

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/project/directory_roles.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "workbench_internal.hpp"
#include <QFileDialog>
#include <QListWidget>
#include <QTimer>

#include <map>
#include <set>

namespace frost::ui {

using workbench_detail::fileIcon;
using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;
using workbench_detail::toUtf8;

// --- Actions ---

void Workbench::addAction(std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                          std::function<bool()> enabled, std::vector<std::string> synonyms) {
    input::ActionSpec spec;
    spec.id = std::string(id);
    spec.name = std::string(name);
    spec.group = std::string(group);
    spec.synonyms = std::move(synonyms);
    spec.context = kWindowRule;
    const QPointer<Workbench> self(this);
    spec.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
        if (self != nullptr && run) {
            run();
        }
    };
    if (enabled) {
        spec.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
            return self != nullptr && enabled();
        };
    } else {
        spec.enabled = [self](const input::ActionContext&) { return self != nullptr; };
    }
    if (Result<input::ActionId> added = m_shell.environment().registry.add(std::move(spec)); !added) {
        log::warn("ui", "workbench action {}: {}", id, added.error().message());
    }
}

void Workbench::registerActions() {
    namespace a = workbench_actions;
    const auto hasEditor = [this] { return m_shell.tabs().currentEditor() != nullptr; };
    const auto hasProject = [this] { return !m_root.empty(); };
    const auto canCreateItem = [this] { return !defaultNewItemDirectory().empty(); };
    const auto canModifyItem = [this] { return !defaultTargetPathForModify().empty(); };
    addAction(a::kNewFile, "New File...", "File", [this] { promptAndCreateNewFile(); }, canCreateItem,
              {"create file", "add file"});
    addAction(a::kNewFolder, "New Folder...", "File", [this] { promptAndCreateNewFolder(); }, canCreateItem,
              {"create folder", "new directory"});
    addAction(a::kRename, "Rename...", "File", [this] { promptAndRenamePath(); }, canModifyItem,
              {"rename file", "rename folder"});
    addAction(a::kDelete, "Delete...", "File", [this] { promptAndDeletePath(); }, canModifyItem,
              {"delete file", "delete folder", "remove file"});
    addAction(a::kFindAction, "Find Action...", "Help", [this] { showFindAction(); }, {}, {"command", "action"});
    addAction(a::kSearchEverywhere, "Search Everywhere", "Navigate", [this] { showSearchEverywhere(); }, {},
              {"find", "search"});
    addAction(a::kGoToFile, "Go to File...", "Navigate", [this] { showGoToFile(); }, {}, {"open file", "file"});
    addAction(a::kRecentFiles, "Recent Files", "Navigate", [this] { showRecentFiles(); }, {}, {"switch file"});
    addAction(a::kRecentLocations, "Recent Locations", "Navigate", [this] { showRecentLocations(); }, {},
              {"recent places"});
    addAction(
        a::kLastEditLocation, "Last Edit Location", "Navigate", [this] { static_cast<void>(goToLastEditLocation()); },
        [this] { return m_lastEditPlace.has_value(); }, {"last edit", "previous edit"});
    addAction(a::kBack, "Back", "Navigate", [this] { goBack(); }, [this] { return m_history.canGoBack(); });
    addAction(a::kForward, "Forward", "Navigate", [this] { goForward(); }, [this] { return m_history.canGoForward(); });
    addAction(
        a::kSelectIn, "Select In...", "Navigate", [this] { static_cast<void>(selectInProjectView()); },
        [hasEditor, hasProject] { return hasEditor() && hasProject(); }, {"locate in project", "reveal in tree"});
    addAction(a::kToggleBookmark, "Toggle Bookmark", "Navigate", [this] { static_cast<void>(toggleBookmark()); },
              hasEditor, {"bookmark"});
    addAction(a::kToggleMnemonicBookmark, "Toggle Bookmark with Mnemonic", "Navigate",
              [this] { static_cast<void>(showMnemonicBookmarkPopup()); }, hasEditor, {"numbered bookmark", "mnemonic"});
    static constexpr std::array<std::string_view, 9> kToggleNumberedIds = {
        a::kToggleBookmark1, a::kToggleBookmark2, a::kToggleBookmark3, a::kToggleBookmark4, a::kToggleBookmark5,
        a::kToggleBookmark6, a::kToggleBookmark7, a::kToggleBookmark8, a::kToggleBookmark9,
    };
    static constexpr std::array<std::string_view, 9> kGotoNumberedIds = {
        a::kGotoBookmark1, a::kGotoBookmark2, a::kGotoBookmark3, a::kGotoBookmark4, a::kGotoBookmark5,
        a::kGotoBookmark6, a::kGotoBookmark7, a::kGotoBookmark8, a::kGotoBookmark9,
    };
    for (int d = 1; d <= 9; ++d) {
        const std::string toggleName = "Toggle Bookmark " + std::to_string(d);
        addAction(
            kToggleNumberedIds[static_cast<std::size_t>(d - 1)], toggleName, "Navigate",
            [this, d] { static_cast<void>(toggleNumberedBookmark(d)); }, hasEditor);
        const std::string gotoName = "Go to Bookmark " + std::to_string(d);
        addAction(
            kGotoNumberedIds[static_cast<std::size_t>(d - 1)], gotoName, "Navigate",
            [this, d] { static_cast<void>(goToNumberedBookmark(d)); },
            [this, d] { return numberedBookmark(d).has_value(); });
    }
    addAction(a::kShowBookmarks, "Show Bookmarks", "Navigate", [this] { static_cast<void>(showBookmarksPopup()); }, {},
              {"bookmarks", "list bookmarks"});
    addAction(a::kBookmarksWindow, "Bookmarks", "Tool Windows", [this] { static_cast<void>(showBookmarksPopup()); }, {},
              {"bookmarks window"});
    addAction(
        a::kNextBookmark, "Next Bookmark", "Navigate", [this] { static_cast<void>(goToNextBookmark()); },
        [this] { return !m_bookmarks.empty(); }, {"next bookmark"});
    addAction(
        a::kPreviousBookmark, "Previous Bookmark", "Navigate", [this] { static_cast<void>(goToPreviousBookmark()); },
        [this] { return !m_bookmarks.empty(); }, {"previous bookmark"});
    addAction(a::kSaveAll, "Save All", "File", [this] { saveAll(); }, {}, {"save"});
    addAction(a::kSaveDocument, "Save Document", "File", [this] { static_cast<void>(saveDocument()); }, hasEditor,
              {"save file"});
    addAction(a::kSaveAs, "Save As...", "File",
              [this] {
                  if (m_services.saveAsHook) {
                      m_services.saveAsHook();
                      return;
                  }
                  const IEditorView* editor = m_shell.tabs().currentEditor();
                  if (editor == nullptr) {
                      return;
                  }
                  const std::string chosen = m_services.promptSaveAsPath
                                                 ? m_services.promptSaveAsPath(editor->path())
                                                 : toUtf8(QDir::toNativeSeparators(QFileDialog::getSaveFileName(
                                                       &m_shell, text("Save As"), nativePath(editor->path()))));
                  if (!chosen.empty()) {
                      static_cast<void>(saveDocumentAs(chosen));
                  }
              },
              hasEditor, {"save file as", "duplicate"});
    // Rider's Reload All from Disk (Ctrl+Alt+Y): every open file, not only the active one.
    addAction(a::kReloadFromDisk, "Reload All from Disk", "File", [this] { reloadAllFromDisk(); }, {},
              {"refresh", "revert", "synchronize"});
    addAction(
        a::kCloseProject, "Close Project", "File",
        [this] {
            if (m_services.closeProjectHook) {
                m_services.closeProjectHook();
            } else {
                static_cast<void>(closeProject());
            }
        },
        hasProject);
    addAction(a::kFileSystemWindow, "File System", "View",
              [this] { m_shell.toolWindows().show(kFileSystemToolWindow); }, {}, {"files", "folder", "tree"});
    addAction(a::kTodoWindow, "TODO", "View",
              [this] {
                  const bool alreadyVisible = m_shell.toolWindows().isVisible(kTodoToolWindow);
                  m_shell.toolWindows().show(kTodoToolWindow);
                  if (alreadyVisible) {
                      refreshTodoItems();
                  }
              },
              {}, {"todo", "fixme", "tasks"});
    // The settings files' stand-in actions, described by frost::settings (settingsActions()).
    for (const settings::SettingsActionInfo& info : settings::settingsActions()) {
        const settings::Scope scope = info.scope;
        std::vector<std::string> synonyms;
        switch (scope) {
        case settings::Scope::ProjectShared: synonyms = {".frost/settings.json", "shared settings"}; break;
        case settings::Scope::ProjectPersonal: synonyms = {".frost/workspace.json", "workspace settings"}; break;
        default: synonyms = {"settings.json", "preferences"}; break;
        }
        std::function<bool()> enabled;
        if (info.needsProject) {
            enabled = hasProject;
        } else {
            enabled = [this] { return !m_services.applicationSettingsFile.empty(); };
        }
        addAction(
            info.id, info.name, info.group,
            [this, scope] {
                if (scope == settings::Scope::Application) {
                    openSettingsFile(m_services.applicationSettingsFile);
                } else if (auto file = projectSettingsFile(scope == settings::Scope::ProjectPersonal); file) {
                    openSettingsFile(*file);
                }
            },
            std::move(enabled), std::move(synonyms));
    }
}

void Workbench::addToolWindow() {
    auto* view = new FileSystemView(&m_shell, m_services.scheduler);
    view->hide();
    m_fileSystem = view;
    connect(view, &FileSystemView::fileActivated, this,
            [this](const QString& path) { static_cast<void>(openFile(toUtf8(path))); });
    connect(view, &FileSystemView::newFileRequested, this,
            [this](const QString& directory) { promptAndCreateNewFile(toUtf8(directory)); });
    connect(view, &FileSystemView::newFolderRequested, this,
            [this](const QString& directory) { promptAndCreateNewFolder(toUtf8(directory)); });
    connect(view, &FileSystemView::renameRequested, this,
            [this](const QString& path) { promptAndRenamePath(toUtf8(path)); });
    connect(view, &FileSystemView::deleteRequested, this,
            [this](const QString& path) { promptAndDeletePath(toUtf8(path)); });
    if (m_services.directoryRoles != nullptr) {
        view->setDirectoryRoleProvider([this](std::string_view directory) {
            return m_roles ? m_roles->decide(directory) : project::DirectoryRoleDecision{};
        });
        connect(view, &FileSystemView::directoryRoleRequested, this, [this](const QString& directory, int role) {
            const std::optional<project::DirectoryRole> chosen =
                role < 0 ? std::nullopt : std::optional(static_cast<project::DirectoryRole>(role));
            if (auto set = setDirectoryRole(toUtf8(directory), chosen); !set)
                notifyError(text("Cannot mark %1").arg(nativePath(toUtf8(directory))), set.error());
        });
    }
    ToolWindowSpec spec;
    spec.id = std::string(kFileSystemToolWindow);
    spec.title = text("File System");
    spec.side = ToolWindowSide::Left;
    spec.iconId = std::string(icons::kNodesFolder);
    spec.shortcutText = shortcutText(workbench_actions::kFileSystemWindow);
    const QPointer<FileSystemView> content(view);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "File System tool window: {}", added.error().message());
    }
}

void Workbench::addTodoToolWindow() {
    auto* list = new QListWidget(&m_shell);
    list->setObjectName(QStringLiteral("todoToolWindowList"));
    list->setFrameShape(QFrame::NoFrame);
    list->setUniformItemSizes(true);
    list->hide();
    m_todoList = list;
    const auto openTodoItem = [this](QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        const QVariantMap map = item->data(Qt::UserRole).toMap();
        const QString path = map.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            return;
        }
        const int line = map.value(QStringLiteral("line"), 1).toInt();
        const int column = map.value(QStringLiteral("column"), 1).toInt();
        static_cast<void>(openFile(toUtf8(path), LinePosition{line, column}));
    };
    connect(list, &QListWidget::itemActivated, this, openTodoItem);
    connect(list, &QListWidget::itemClicked, this, openTodoItem);
    connect(&m_shell.toolWindows(), &ToolWindowManager::visibilityChanged, this,
            [this](const QString& id, bool visible) {
                if (visible && id == fromUtf8(kTodoToolWindow)) {
                    refreshTodoItems();
                }
            });
    ToolWindowSpec spec;
    spec.id = std::string(kTodoToolWindow);
    spec.title = text("TODO");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kStatusInfo);
    spec.shortcutText = shortcutText(workbench_actions::kTodoWindow);
    const QPointer<QListWidget> content(list);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "TODO tool window: {}", added.error().message());
    }
}

QString Workbench::shortcutText(std::string_view actionId) const {
    const ShellEnvironment& env = m_shell.environment();
    const std::optional<input::ActionId> id = env.registry.find(actionId);
    if (!id) {
        return {};
    }
    const std::optional<input::Sequence> shortcut = env.keymap.primaryShortcut(*id);
    return shortcut ? fromUtf8(input::toDisplayText(*shortcut, env.keymap.layout(), env.keymap.platform())) : QString();
}

void Workbench::applyTodoEntries(const std::vector<TodoEntry>& entries) {
    if (m_todoList == nullptr) {
        return;
    }
    m_todoList->setUpdatesEnabled(false);
    m_todoList->clear();
    for (const TodoEntry& entry : entries) {
        auto* item = new QListWidgetItem(QStringLiteral("%1:%2  %3")
                                             .arg(nativePath(entry.relativePath))
                                             .arg(entry.line)
                                             .arg(fromUtf8(entry.snippet).trimmed()));
        item->setIcon(fileIcon(nav::baseName(entry.path)));
        QVariantMap data;
        data.insert(QStringLiteral("path"), fromUtf8(entry.path));
        data.insert(QStringLiteral("line"), entry.line);
        data.insert(QStringLiteral("column"), entry.column);
        item->setData(Qt::UserRole, data);
        m_todoList->addItem(item);
    }
    m_todoList->setUpdatesEnabled(true);
}

void Workbench::refreshTodoItems() {
    if (m_todoList == nullptr) {
        return;
    }

    static constexpr std::size_t kMaxCandidateFiles = 2048;
    static constexpr std::size_t kMaxTodoItems = 1500;
    static constexpr std::uint64_t kMaxFileBytes = 256ULL * 1024ULL;

    std::vector<std::string> candidatePaths;
    std::set<std::string, std::less<>> seen;
    const auto addCandidate = [&](const std::string& path) {
        if (candidatePaths.size() >= kMaxCandidateFiles || !isTodoScannablePath(path)) {
            return;
        }
        if (seen.insert(path).second) {
            candidatePaths.push_back(path);
        }
    };

    if (m_solutionFiles) {
        for (const std::string& path : *m_solutionFiles) {
            addCandidate(path);
        }
    }
    if (m_files) {
        for (const std::string& path : m_files->candidates()) {
            addCandidate(path);
        }
    }
    for (const auto& doc : m_services.saveManager.documents()) {
        if (doc != nullptr && !doc->path().empty()) {
            addCandidate(doc->path());
        }
    }
    for (EditorTabGroup* group : m_shell.tabs().groups()) {
        if (group == nullptr) {
            continue;
        }
        for (int i = 0; i < group->count(); ++i) {
            if (const IEditorView* editor = group->editor(i); editor != nullptr && !editor->path().empty()) {
                addCandidate(editor->path());
            }
        }
    }

    std::map<std::string, std::string, std::less<>> openBuffers;
    for (const auto& doc : m_services.saveManager.documents()) {
        if (doc != nullptr && !doc->path().empty()) {
            openBuffers.emplace(doc->path(), doc->document().text().text());
        }
    }

    auto runScan = [root = m_root](io::IFileSystem& fs, const std::vector<std::string>& paths,
                                   const std::map<std::string, std::string, std::less<>>& buffers,
                                   const std::stop_token& token) -> std::vector<TodoEntry> {
        std::vector<TodoEntry> entries;
        for (const std::string& path : paths) {
            if (token.stop_requested() || entries.size() >= kMaxTodoItems) {
                break;
            }
            std::string content;
            if (const auto it = buffers.find(path); it != buffers.end()) {
                content = it->second;
            } else {
                auto status = fs.status(path);
                if (!status || status->type != io::FileType::Regular || status->size == 0 ||
                    status->size > kMaxFileBytes) {
                    continue;
                }
                auto read = fs.readFile(path);
                if (!read) {
                    continue;
                }
                content = std::move(*read);
            }
            if (!mayContainTodoTag(content)) {
                continue;
            }
            const std::string rel = root.empty() ? path : nav::relativeTo(root, path);
            std::size_t pos = 0;
            int lineNum = 1;
            while (pos <= content.size() && entries.size() < kMaxTodoItems) {
                const std::size_t nl = content.find('\n', pos);
                std::string_view line = nl == std::string::npos ? std::string_view(content).substr(pos)
                                                                : std::string_view(content).substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                if (const auto match = findTodoTag(line)) {
                    std::string_view snippet = line.substr(match->first);
                    if (snippet.size() > 240) {
                        snippet = snippet.substr(0, 240);
                    }
                    entries.push_back(
                        TodoEntry{path, rel, std::string(snippet), lineNum, static_cast<int>(match->first) + 1});
                }
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
                ++lineNum;
            }
        }
        return entries;
    };

    if (m_services.scheduler == nullptr) {
        applyTodoEntries(runScan(m_services.fs, candidatePaths, openBuffers, {}));
        return;
    }

    if (m_todoScan) {
        m_todoStop.request_stop();
        m_todoRescanQueued = true;
        m_scanTimer->start();
        return;
    }

    m_todoStop = std::stop_source();
    m_todoScanGeneration = m_projectGeneration;
    auto& fs = m_services.fs;
    const std::stop_token token = m_todoStop.get_token();
    auto task = [](io::IFileSystem& fileSystem, auto scanner, std::vector<std::string> paths,
                   std::map<std::string, std::string, std::less<>> buffers,
                   std::stop_token stopToken) -> sched::Task<std::vector<TodoEntry>> {
        co_return scanner(fileSystem, paths, buffers, stopToken);
    };
    m_todoScan.emplace(
        m_services.scheduler->spawn(sched::Lane::Background, task(fs, std::move(runScan), std::move(candidatePaths),
                                                                  std::move(openBuffers), token)));
    m_scanTimer->start();
}

} // namespace frost::ui
