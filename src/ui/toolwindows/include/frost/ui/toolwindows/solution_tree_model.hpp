// SPDX-License-Identifier: Apache-2.0
//
// SolutionTreeModel: the Solution Explorer's item model over a project::SolutionModel (W1,
// ADR 0039), plus the speed-search matcher and the filter model the explorer narrows the tree with.
// Both models work without a view, so tests check the tree, its icons and its search directly.
//
/*
 * Tree. One top-level row, the solution; below it the SolutionModel's own tree (solution folders,
 * projects, .vcxproj.filters folders, files) in its order: folders and filters first, then projects,
 * then files, each by name with ASCII case ignored (frost/project/solution_model.hpp). Only project
 * items are shown ("Show All Files" off; TODO(M2): Show All Files once the project model lists the
 * project directories). Projects that failed to load stay in the tree with a secondary "(unavailable)"
 * or "(not supported yet)" and the reason in the tooltip.
 *
 * Icons come from the theme's icon atlas by id (IconIdRole names it): the solution
 * "toolwindow.structure", solution folders and filters "nodes.folder" (the delegate swaps in
 * "nodes.folder-open" while expanded), loaded projects "build.package", unsupported projects
 * "status.warning", unavailable ones "status.failed", C and C++ sources and headers
 * "nodes.file-code" and other files "nodes.file".
 *
 * Colours: with a file colour provider set (the VCS status colours, M5, ADR 0048), a file node's
 * Qt::ForegroundRole is the provider's colour for its path; refreshFileColors() tells the views when
 * the colours moved.
 *
 * Index layout: internalId() is the NodeId, so an index stays meaningful for the model's solution;
 * setSolution() resets the model. GUI thread only. O(1) per data() call; setSolution is O(nodes).
 */
#pragma once

#include "frost/project/solution_model.hpp"

#include <QAbstractItemModel>
#include <QColor>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStyledItemDelegate>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::ui {

namespace detail {
class IconCache;
} // namespace detail

// The atlas icon id for a node of `model` (header comment). Pure.
[[nodiscard]] std::string_view solutionNodeIconId(const project::SolutionModel& model, project::NodeId id) noexcept;

// Speed search's match rule: `query` occurs in `text`, case ignored; an empty query matches nothing.
[[nodiscard]] bool speedSearchMatches(QStringView text, QStringView query) noexcept;

class SolutionTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role : int {
        NodeIdRole = Qt::UserRole + 1, // quint32: the project::NodeId.
        KindRole,                      // int: project::NodeKind.
        PathRole,                      // QString: the absolute path; "" for folders and filters.
        ProjectIdRole,                 // int: the project::ProjectId of projects, filters and project files; -1.
        IconIdRole,                    // QString: the atlas icon id (header comment).
        SecondaryTextRole,             // QString: the grey suffix ("5 projects", "(unavailable)"), or "".
        KeyRole,                       // QString: project::Node::key, stable across reloads.
        FilterPathRole,                // QString: project::Node::filterPath for Filter nodes.
    };

    explicit SolutionTreeModel(QObject* parent = nullptr);
    ~SolutionTreeModel() override;

    // Shows `solution` (nullptr: an empty tree). Resets the model.
    void setSolution(std::shared_ptr<const project::SolutionModel> solution);
    [[nodiscard]] const std::shared_ptr<const project::SolutionModel>& solution() const noexcept { return m_solution; }

    // The index of a node; invalid without a solution or for an id out of range. O(1).
    [[nodiscard]] QModelIndex indexOf(project::NodeId id) const;
    // The node an index of this model shows; nullopt for an invalid index.
    [[nodiscard]] std::optional<project::NodeId> nodeOf(const QModelIndex& index) const;
    // The first file node at `path` (absolute), as SolutionModel::findFileNode finds it; invalid if none.
    [[nodiscard]] QModelIndex indexOfPath(std::string_view path) const;
    // Every node in display order (depth first, the solution first), for speed search.
    [[nodiscard]] std::vector<project::NodeId> displayOrder() const;

    // The colour of a file's name (nullopt: the default text colour), queried in data().
    using FileColorProvider = std::function<std::optional<QColor>(std::string_view absolutePath)>;
    // Sets (or clears, with an empty function) the file colours and repaints. O(nodes).
    void setFileColorProvider(FileColorProvider provider);
    // Tells the views that every file colour may have changed (dataChanged for Qt::ForegroundRole). O(nodes).
    void refreshFileColors();

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                       const QModelIndex& parent) const override;
    [[nodiscard]] bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                    const QModelIndex& parent) override;

signals:
    void filesDroppedOnFilter(const QString& projectPath, const QStringList& filePaths,
                              const QString& targetFilterPath);
    void physicalFilesDroppedOnDirectory(const QString& projectPath, const QStringList& filePaths,
                                         const QString& targetDirectoryPath);
    void projectDroppedOnSolutionFolder(const QString& projectPath, const QString& targetFolderId);

private:
    std::shared_ptr<const project::SolutionModel> m_solution;
    std::vector<int> m_rowInParent; // By NodeId: the node's row among its parent's children.
    std::unique_ptr<detail::IconCache> m_icons;
    FileColorProvider m_fileColors;
};

/*
 * The speed-search filter over a SolutionTreeModel: with a query set, only nodes whose name matches
 * it and their ancestors are shown (recursive filtering); without one, everything. GUI thread only;
 * each query change re-filters in O(nodes).
 */
class SolutionFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit SolutionFilterModel(QObject* parent = nullptr);

    void setQuery(const QString& query);
    [[nodiscard]] const QString& query() const noexcept { return m_query; }

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString m_query;
};

/*
 * Paints a tree row: the open-folder icon for expanded folders, the name, and the secondary text in
 * the theme's secondary label colour after it. GUI thread only.
 */
class SolutionItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SolutionItemDelegate(QObject* parent = nullptr);
    ~SolutionItemDelegate() override;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;

private:
    std::unique_ptr<detail::IconCache> m_icons;
};

} // namespace frost::ui
