// SPDX-License-Identifier: Apache-2.0
//
// FileSystemView: the File System tool window's content (ADR 0042): the project folder as a tree
// (nav::ProjectFiles, the gitignore-aware scan), file icons, Rider's speed search (typing in the
// tree filters to the matching names and selects the first), and activation (Enter, double-click)
// that opens the file.
#pragma once

#include "frost/project/directory_roles.hpp"
#include "frost/ui/nav/project_files.hpp"

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;

class QTreeView;

namespace frost::ui {

class ProjectTreeModel;

/*
 * The tree is rebuilt by setFiles() (after every scan); expansion and the selection survive a
 * rebuild by path. Speed search matches names case-insensitively by substring or by word starts
 * ("mc" matches "main.cpp", search::scoreWord); non-matching rows are hidden while it is active
 * and Escape ends it. GUI thread only.
 */
class FileSystemView final : public QWidget {
    Q_OBJECT

public:
    // Roles on tree()->model(); paths are absolute and DirectoryRole is a bool.
    enum Role : int { PathRole = Qt::UserRole + 1, DirectoryRole };
    explicit FileSystemView(QWidget* parent = nullptr, sched::IScheduler* scheduler = nullptr);
    ~FileSystemView() override;

    // Shows `files` (empty root: the "no folder" message).
    void setFiles(std::shared_ptr<const nav::ProjectFiles> files);

    // Speed search: sets the query (empty ends it), selects the first match and returns the number
    // of matching files and folders.
    int setSpeedSearch(const QString& query);
    [[nodiscard]] QString speedSearch() const;
    // The path of the selected row, or empty.
    [[nodiscard]] std::string selectedPath() const;
    // All selected paths in the tree (supports multi-selection).
    [[nodiscard]] QStringList selectedPaths() const;
    // True when the selected row is a directory.
    [[nodiscard]] bool selectedIsDirectory() const;
    // The directory of the selected row (its own path for a folder, parent for a file), else root.
    [[nodiscard]] std::string selectedDirectory() const;
    // Selects the row of `path` (expanding its parents); false when the tree does not have it.
    bool select(const std::string& path);
    // Selects `path` now, or remembers it to select as soon as a scan publishes it.
    void selectWhenAvailable(std::string path);
    // Builds the context menu for `index` (invalid index: the root folder's menu); caller owns it.
    [[nodiscard]] QMenu* createContextMenu(const QModelIndex& index, QWidget* parent = nullptr);
    // Activates the selected row: a file emits fileActivated(), a folder toggles.
    void activateSelected();
    // The visible rows' names in tree order (expanded folders' children included), for tests.
    [[nodiscard]] QStringList visibleNames() const;

    using FileColorProvider = std::function<std::optional<QColor>(std::string_view)>;
    void setFileColorProvider(FileColorProvider provider);
    void refreshFileColors();
    void setCanPasteProvider(std::function<bool()> provider) { m_canPaste = std::move(provider); }
    // The decision for a folder (directory roles, UX-23), for the "Mark Directory as" submenu of a
    // folder below the root; empty: no such submenu.
    using DirectoryRoleProvider = std::function<project::DirectoryRoleDecision(std::string_view directory)>;
    void setDirectoryRoleProvider(DirectoryRoleProvider provider) { m_directoryRoles = std::move(provider); }

    [[nodiscard]] QTreeView* tree() const noexcept { return m_tree; }
    // Index in the displayed tree, invalid when absent or filtered out. GUI thread only.
    [[nodiscard]] QModelIndex indexOfPath(const std::string& path) const;
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_search; }

signals:
    void fileActivated(const QString& path);
    void newFileRequested(const QString& directory);
    void newFolderRequested(const QString& directory);
    void renameRequested(const QString& path, bool isDirectory);
    void deleteRequested(const QString& path, bool isDirectory);
    void copyPathsRequested(const QStringList& paths);
    void cutPathsRequested(const QStringList& paths);
    void pasteIntoDirectoryRequested(const QString& targetDirectory);
    void duplicatePathRequested(const QString& path, bool isDirectory);
    void movePathsToDirectoryRequested(const QStringList& sourcePaths, const QString& targetDirectory);
    void openContainingFolderRequested(const QString& path);
    void openInTerminalRequested(const QString& directoryPath);
    void addPathsToProjectRequested(const QStringList& paths);
    void addExistingFilesToProjectRequested(const QString& directory);
    void addExistingFolderToProjectRequested(const QString& directory);
    // "Mark Directory as": `role` is a project::DirectoryRole value, or -1 to drop the override.
    void directoryRoleRequested(const QString& directory, int role);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void activate(const QModelIndex& index);
    void scheduleFilteredFiles(std::shared_ptr<const nav::ProjectFiles> files);
    void startFilter();
    void pollFilter();
    void publishFilteredFiles();
    void queueExpansionRestore(const std::string& root);
    void restoreScrollPosition();
    // Applies the speed search to `parent`'s subtree; returns whether any row in it matches.
    bool filter(const QModelIndex& parent, const std::string& query, int& matches, QModelIndex& first);

    struct FilterState;
    std::unique_ptr<FilterState> m_filterState;
    std::set<std::string> m_expanded;
    std::optional<QPoint> m_restoreScroll;
    std::string m_pendingSelectPath;
    bool m_displayCurrent = true;
    FileColorProvider m_fileColors;
    std::function<bool()> m_canPaste;
    DirectoryRoleProvider m_directoryRoles;
    QTreeView* m_tree;
    ProjectTreeModel* m_model;
    QLineEdit* m_search;
    QLabel* m_empty;
};

} // namespace frost::ui
