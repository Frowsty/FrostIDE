// SPDX-License-Identifier: Apache-2.0
//
// AstViewPopup (frost/ui/lang/ast_view.hpp): the tree of clangd's textDocument/ast answer.

#include "frost/ui/lang/ast_view.hpp"

#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

// Item data: whether the row has a range, and its start and end (line, character) pairs.
constexpr int kHasRangeRole = Qt::UserRole + 1;
constexpr int kStartRole = Qt::UserRole + 2;
constexpr int kEndRole = Qt::UserRole + 3;

[[nodiscard]] QString qs(const std::string& text) { return QString::fromStdString(text); }

} // namespace

AstViewPopup::AstViewPopup(QWidget* parent) : QFrame(parent), m_title(new QLabel(this)), m_tree(new QTreeWidget(this)) {
    setObjectName(QStringLiteral("astViewPopup"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setFrameShape(QFrame::Box);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    m_title->setObjectName(QStringLiteral("astViewTitle"));
    m_tree->setObjectName(QStringLiteral("astViewTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Node"), QStringLiteral("Role"), QStringLiteral("Range")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setUniformRowHeights(true);
    layout->addWidget(m_title);
    layout->addWidget(m_tree, 1);
    resize(640, 420);
    m_tree->installEventFilter(this);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem*, int) { activateCurrent(); });
}

QString AstViewPopup::title() const { return m_title->text(); }

QTreeWidgetItem* AstViewPopup::addNode(QTreeWidgetItem* parent, const fl::AstNode& node, int depth) {
    auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    ++m_nodes;
    QString label = qs(node.kind.empty() ? node.role : node.kind);
    if (!node.detail.empty()) {
        label += QStringLiteral(" (") + qs(node.detail) + QLatin1Char(')');
    }
    item->setText(0, label);
    item->setText(1, qs(node.role));
    if (node.range) {
        item->setText(2, QStringLiteral("%1:%2").arg(node.range->start.line + 1).arg(node.range->start.character + 1));
        item->setData(0, kHasRangeRole, true);
        item->setData(0, kStartRole,
                      QPoint(static_cast<int>(node.range->start.line), static_cast<int>(node.range->start.character)));
        item->setData(0, kEndRole,
                      QPoint(static_cast<int>(node.range->end.line), static_cast<int>(node.range->end.character)));
    }
    if (!node.arcana.empty()) {
        item->setToolTip(0, qs(node.arcana));
    }
    for (const fl::AstNode& child : node.children) {
        addNode(item, child, depth + 1);
    }
    if (node.truncated) {
        auto* omitted = new QTreeWidgetItem(item);
        omitted->setText(0, QStringLiteral("(deeper nodes omitted)"));
        omitted->setFlags(omitted->flags() & ~Qt::ItemIsSelectable);
    }
    item->setExpanded(depth < 2);
    return item;
}

void AstViewPopup::setAst(const QString& title, const fl::AstNode& root) {
    m_title->setText(title);
    m_tree->clear();
    m_nodes = 0;
    QTreeWidgetItem* top = addNode(nullptr, root, 0);
    m_tree->setCurrentItem(top);
}

void AstViewPopup::popup(QWidget* over) {
    if (over != nullptr) {
        const QPoint centre = over->mapToGlobal(over->rect().center());
        move(centre.x() - width() / 2, std::max(over->mapToGlobal(QPoint(0, 0)).y() + 40, centre.y() - height() / 2));
    }
    show();
    raise();
    activateWindow();
    m_tree->setFocus();
}

void AstViewPopup::activateCurrent() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr || !item->data(0, kHasRangeRole).toBool()) {
        return;
    }
    const QPoint start = item->data(0, kStartRole).toPoint();
    const QPoint end = item->data(0, kEndRole).toPoint();
    hide();
    emit rangeActivated(
        fl::LspRange{fl::LspPosition{static_cast<std::uint32_t>(start.x()), static_cast<std::uint32_t>(start.y())},
                     fl::LspPosition{static_cast<std::uint32_t>(end.x()), static_cast<std::uint32_t>(end.y())}});
}

bool AstViewPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateCurrent();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace frost::ui::lang
