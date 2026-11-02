// SPDX-License-Identifier: Apache-2.0
//
// CMakeTreeModel: the Qt item model of an opened CMake folder for the Solution Explorer (ADR 0046),
// an adapter over frost::project's CMakeViewModel (project, targets, source folders, generated and
// external sources, the "CMake" group). One column; the view model's root is the single top-level
// row. The W1 workbench puts it into the Solution Explorer's tree view.
/*
 * Roles: Qt::DisplayRole (the label), Qt::ToolTipRole (the path; generated sources say they come
 * from the build), Qt::FontRole (italic for generated sources) and the custom roles below.
 * Rows: a node's row is its index in its parent's children; internalId is the node id.
 *
 * setViewModel resets the model (beginResetModel/endResetModel). A view keeps expansion across a
 * reconfigure by mapping its expanded indexes to keys (KeyRole) before and indexForKey after.
 *
 * Threads: a QObject of the UI thread; use it there only. The view model is immutable and shared.
 */
#pragma once

#include "frost/project/cmake_view_model.hpp"

#include <QAbstractItemModel>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace frost::ui::cmake {

class CMakeTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role {
        PathRole = Qt::UserRole + 1, // QString: the absolute path (files, folders, the project), or "".
        KindRole,                    // int: project::CMakeViewNodeKind.
        KeyRole,                     // QString: the stable node key (CMakeViewNode::key).
        GeneratedRole,               // bool: a source the build generates.
        TargetIndexRole,             // int: the index into CMakeProject::targets, or -1.
    };

    explicit CMakeTreeModel(QObject* parent = nullptr);

    // Shows `model`; nullptr shows nothing (no rows).
    void setViewModel(std::shared_ptr<const project::CMakeViewModel> model);
    [[nodiscard]] const std::shared_ptr<const project::CMakeViewModel>& viewModel() const noexcept { return m_model; }

    // The index of the node with `key`, or an invalid index. O(log n) plus the depth.
    [[nodiscard]] QModelIndex indexForKey(std::string_view key) const;
    // The view-model node of `index`, or nullopt for an invalid index.
    [[nodiscard]] std::optional<std::size_t> nodeId(const QModelIndex& index) const;

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    [[nodiscard]] QModelIndex indexOfNode(std::size_t id) const;

    std::shared_ptr<const project::CMakeViewModel> m_model;
    std::vector<int> m_rows; // Node id -> its row under its parent.
};

} // namespace frost::ui::cmake
