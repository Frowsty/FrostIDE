// SPDX-License-Identifier: Apache-2.0
//
// SolutionExplorer: the Solution Explorer tool window (W1, ADR 0039), a standalone QWidget the
// shell's ToolWindowManager docks: the solution tree, Rider-like speed search, file activation,
// the context menu with the project build commands, Open Containing Folder and Copy Path, and the
// selection API behind "autoscroll from source".
//
/*
 * Speed search. Typing a printable character (no Ctrl, Alt or Meta; not a leading space) while the
 * tree has focus starts it: the query shows in a field above the tree, the tree is filtered to the
 * matching nodes and their ancestors, expanded, and the first match (in display order) is selected.
 * More characters narrow the query, Backspace shortens it (and ends the search when it empties),
 * Up and Down step through the matches, Enter opens the match and ends the search, Escape ends it,
 * and any other key, a click or losing focus ends it before the key or click takes effect. A query
 * without matches turns the field's text red and keeps the last matches shown. Ending restores the
 * expansion from before the search and keeps the match selected and revealed.
 *
 * Activation. Double-click or Enter on a file emits openFile(absolute path); Enter on a folder or
 * project toggles it (double-click does so through QTreeView).
 *
 * Commands. The context menu offers Open (files); Build, Rebuild and Clean for the solution or a
 * project, through the BuildController (disabled without one, during a build, and for projects
 * that are not loaded); Set as Startup Project for a loaded project, when a handler is set (the
 * run service's, ADR 0040); Open Containing Folder (revealInFileManager through the IProcessLauncher;
 * disabled without one); Copy Path (the native path to the clipboard). A failed command emits
 * commandFailed().
 *
 * Refresh. setSolution() (or the controller's solutionChanged()) rebuilds the tree; for a reload of
 * the same solution file the expansion and the selection come back by node key.
 *
 * GUI thread only; the controller and the launcher are not owned and must outlive the explorer (or
 * be reset to null first).
 */
#pragma once

#include "frost/project/solution_model.hpp"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class QKeyEvent;
class QLineEdit;
class QMenu;
class QModelIndex;
class QTreeView;

namespace frost::io {
class IProcessLauncher;
} // namespace frost::io

namespace frost::ui {

class BuildController;
class SolutionFilterModel;
class SolutionTreeModel;

class SolutionExplorer final : public QWidget {
    Q_OBJECT

public:
    explicit SolutionExplorer(QWidget* parent = nullptr);
    ~SolutionExplorer() override;

    // Binds the build commands and shows the controller's solution from now on (nullptr unbinds).
    void setController(BuildController* controller);
    // The launcher for Open Containing Folder (nullptr disables the command).
    void setProcessLauncher(io::IProcessLauncher* launcher) noexcept { m_launcher = launcher; }
    // What Set as Startup Project does with the project; empty: the command is not offered.
    void setStartupProjectHandler(std::function<void(project::ProjectId)> handler) {
        m_startupHandler = std::move(handler);
    }

    // Shows `solution` (nullptr: nothing); see the header comment for reloads.
    void setSolution(std::shared_ptr<const project::SolutionModel> solution);
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const;

    // Selects and reveals the file node at `path` (absolute) for "autoscroll from source", ending a
    // speed search first. Returns false, changing nothing, when the solution has no such file.
    bool selectPath(std::string_view path);
    // The selected node and its path ("" for folders, filters and no selection).
    [[nodiscard]] std::optional<project::NodeId> currentNode() const;
    // The project of the selected project node, if a project node is selected.
    [[nodiscard]] std::optional<project::ProjectId> currentProject() const;
    [[nodiscard]] QString currentPath() const;

    [[nodiscard]] bool isSpeedSearchActive() const noexcept { return m_searchActive; }
    [[nodiscard]] const QString& speedSearchText() const noexcept { return m_query; }
    // The current matches in display order (empty when the search is off or nothing matches).
    [[nodiscard]] const std::vector<project::NodeId>& speedSearchMatches() const noexcept { return m_matches; }
    // Ends a running speed search (header comment); no-op otherwise.
    void endSpeedSearch();

    // Builds the context menu for `index`, an index of view()->model(); the caller owns the menu.
    // An invalid index gives an empty menu.
    [[nodiscard]] QMenu* createContextMenu(const QModelIndex& index, QWidget* parent = nullptr);

    [[nodiscard]] QTreeView* view() const noexcept { return m_view; }
    [[nodiscard]] SolutionTreeModel* model() const noexcept { return m_model; }
    [[nodiscard]] SolutionFilterModel* filterModel() const noexcept { return m_filter; }
    [[nodiscard]] QLineEdit* speedSearchField() const noexcept { return m_searchField; }

signals:
    // A file was activated (double-click, Enter, or Open in the context menu).
    void openFile(const QString& absolutePath);
    void commandFailed(const QString& message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] bool handleKey(QKeyEvent& event);
    void activate(const QModelIndex& index, bool fromKeyboard);
    void beginSpeedSearch();
    void updateSpeedSearch(const QString& query);
    void stepMatch(int delta);
    void selectNode(project::NodeId id);
    [[nodiscard]] QModelIndex proxyIndexOf(project::NodeId id) const;
    [[nodiscard]] QStringList expandedKeys() const;
    void restoreExpansion(const QStringList& keys);
    void openContainingFolder(const QString& path);

    QTreeView* m_view = nullptr;
    QLineEdit* m_searchField = nullptr;
    SolutionTreeModel* m_model = nullptr;
    SolutionFilterModel* m_filter = nullptr;
    BuildController* m_controller = nullptr;
    io::IProcessLauncher* m_launcher = nullptr;
    std::function<void(project::ProjectId)> m_startupHandler;
    QMetaObject::Connection m_solutionConnection;

    bool m_searchActive = false;
    QString m_query;
    std::vector<project::NodeId> m_matches;
    std::size_t m_matchIndex = 0;
    QStringList m_expandedBeforeSearch;
};

} // namespace frost::ui
