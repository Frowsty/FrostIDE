// SPDX-License-Identifier: Apache-2.0
//
// The AST view (CPP-5): a popup tree of the clang AST node clangd returns for the caret or the
// selection (textDocument/ast, frost/lang/document_features.hpp), with each node's kind, detail
// and role, and clangd's dump line ("arcana") as its tooltip. Activating a node (Enter or a double
// click) emits its source range, which the language controller selects in the editor. Every row
// comes from clangd's answer; nothing is inferred from the text.
/*
 * Columns: "Node" (kind, then the detail in parentheses when clangd sent one), "Role" and "Range"
 * (1-based line:column of the start, from the server's range). The root and its first two levels
 * are expanded. A node whose children were cut at kMaxAstDepth shows a "(deeper nodes omitted)"
 * child row, so the cut is visible.
 *
 * Threads and lifetime: UI thread only. A Qt::Popup frame, hidden by Escape or a click outside;
 * owned by LanguageController.
 */
#pragma once

#include "frost/lang/document_features.hpp"

#include <QFrame>
#include <QString>

#include <cstddef>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace frost::ui::lang {

class AstViewPopup final : public QFrame {
    Q_OBJECT

public:
    explicit AstViewPopup(QWidget* parent = nullptr);

    // Replaces the tree with `root` under `title` ("AST at Foo.cpp:12"). O(nodes).
    void setAst(const QString& title, const ::frost::lang::AstNode& root);
    // Shows the popup centred over `over` (the editor) and focuses the tree.
    void popup(QWidget* over);

    [[nodiscard]] QTreeWidget* tree() const noexcept { return m_tree; }
    [[nodiscard]] QString title() const;
    // The number of node rows shown (the omitted-children markers excluded).
    [[nodiscard]] std::size_t nodeCount() const noexcept { return m_nodes; }

signals:
    // A node with a range was activated: select `range` (LSP positions of the document).
    void rangeActivated(::frost::lang::LspRange range);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void activateCurrent();
    QTreeWidgetItem* addNode(QTreeWidgetItem* parent, const ::frost::lang::AstNode& node, int depth);

    QLabel* m_title = nullptr;
    QTreeWidget* m_tree = nullptr;
    std::size_t m_nodes = 0;
};

} // namespace frost::ui::lang
