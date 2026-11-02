// SPDX-License-Identifier: Apache-2.0
//
// The Debug tool window's item models (PLAN.md M4, ADR 0050): FramesModel (the stopped thread's
// frames, loaded a page at a time) and VariablesModel (the Variables tree, the Watches and the
// Evaluate dialog's result: lazy children, paged arrays, values as lldb-dap formats them). Both read
// a DebugController and refresh on its signals.
/*
 * VariablesModel. In Scopes mode the roots are the selected frame's scopes (reloaded on every stop
 * and frame change); in Watches mode they are the watch expressions, evaluated again on every stop
 * (context "watch"; the Evaluate dialog uses "repl"). A node with a variablesReference has children
 * that load when the view expands it (canFetchMore/fetchMore). Children of an array with more than
 * kPage indexed elements load kPage at a time; a "Load more" row fetches the next page (loadMore).
 * Columns: Name, Value, Type. Results of requests that a reset or a removal overtook are dropped.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/debug/debug_session.hpp"

#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace frost::ui {

class DebugController;

class FramesModel final : public QAbstractListModel {
    Q_OBJECT

public:
    // The frame id of a row; -1 for the "Load more frames" row.
    static constexpr int kFrameIdRole = Qt::UserRole + 1;

    explicit FramesModel(DebugController& controller, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    // The row of the selected frame, or -1.
    [[nodiscard]] int selectedRow() const;
    // Whether `row` is the "Load more frames" row.
    [[nodiscard]] bool isLoadMoreRow(int row) const;

private:
    void refresh();

    DebugController& m_controller;
};

class VariablesModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum class Mode : std::uint8_t { Scopes, Watches };
    // Children asked for per page of a large array.
    static constexpr std::int64_t kPage = 100;

    VariablesModel(DebugController& controller, Mode mode, QObject* parent = nullptr);
    ~VariablesModel() override;

    // Watches mode: adds `expression` (evaluated at once when paused) and removes a root row.
    void addWatch(const QString& expression);
    void removeWatch(int row);
    [[nodiscard]] QStringList watches() const;
    // Watches mode: the expressions with `context` ("watch", or "repl" for the Evaluate dialog).
    void setEvaluateContext(QString context) { m_context = std::move(context); }
    // Re-reads the roots: the scopes of the selected frame, or every watch.
    void reload();

    // The "Load more" row at `index` fetches the next page; other rows are ignored.
    void loadMore(const QModelIndex& index);
    [[nodiscard]] bool isLoadMoreRow(const QModelIndex& index) const;
    // The value shown at `index` (Copy Value).
    [[nodiscard]] QString valueAt(const QModelIndex& index) const;
    // Requests still out (tests wait for 0).
    [[nodiscard]] int pendingRequests() const noexcept { return m_pending; }

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

private:
    struct Node;

    [[nodiscard]] Node* nodeFor(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex indexFor(const Node* node, int column = 0) const;
    [[nodiscard]] Node* find(std::uint64_t id) const;
    Node* adopt(std::unique_ptr<Node> node);
    void forget(Node& node);
    void clearChildren(Node& node);
    void fetch(Node& node, std::int64_t start, std::int64_t count);
    void addChildren(std::uint64_t id, Result<debug::VariableList> list, std::int64_t start, std::int64_t count);
    void evaluateWatch(Node& node);
    void clearRoots();

    DebugController& m_controller;
    Mode m_mode;
    QString m_context = QStringLiteral("watch");
    std::unique_ptr<Node> m_root;
    std::unordered_map<std::uint64_t, Node*> m_nodes;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_generation = 0; // Bumped by reload(); scope answers of older loads are dropped.
    int m_pending = 0;
};

} // namespace frost::ui
