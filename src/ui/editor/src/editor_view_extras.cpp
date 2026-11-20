// SPDX-License-Identifier: Apache-2.0
// UX-7: one bounded sticky scope label from the already parsed current-version outline.
#include "frost/ui/editor/editor_view.hpp"
#include "frost/syntax/syntax_tree.hpp"
#include <QLabel>
#include <QTimer>
#include <algorithm>
#include <chrono>
namespace frost::ui {
void EditorView::initializeEditorExtras() {
    m_stickyScope = new QLabel(viewport());
    m_stickyScope->setObjectName(QStringLiteral("editorStickyScope"));
    m_stickyScope->setTextFormat(Qt::PlainText);
    m_stickyScope->setAutoFillBackground(true);
    m_stickyScope->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_stickyScope->hide();
    auto* timer = new QTimer(this);
    timer->setInterval(150);
    connect(timer, &QTimer::timeout, this, &EditorView::refreshStickyScope);
    timer->start();
}
void EditorView::refreshStickyScope() {
    if (!m_stickyScope) return;
    m_stickyScope->hide();
    if (!isVisible() || !m_syntax || firstVisibleRow() == 0) return;
    const auto tree = m_syntax->tree();
    const auto snapshot = m_editor->text();
    if (tree.empty() || tree.stale() || tree.version() != snapshot.version()) return;
    syntax::QueryLimits limits;
    limits.budget = std::chrono::milliseconds(1);
    limits.maxResults = 2048;
    limits.maxDepth = 16;
    const auto outline = tree.outline(limits);
    // An incomplete traversal cannot prove the nearest enclosing scope. Show nothing until complete.
    if (!outline.complete || tree.version() != m_editor->text().version()) return;
    const auto row = m_editor->display().rowInfo(text::DisplayRow{firstVisibleRow()});
    const syntax::OutlineItem* selected = nullptr;
    for (const auto& item : outline.items) {
        if (item.range.begin >= row.buffer.begin) continue;
        const bool heading = item.kind == syntax::OutlineKind::Heading;
        if (heading || row.buffer.begin < item.range.end)
            if (!selected || item.range.begin >= selected->range.begin) selected = &item;
    }
    if (!selected || selected->name.empty()) return;
    m_stickyScope->setText(QString::fromStdString(selected->name));
    m_stickyScope->setFont(font());
    m_stickyScope->setGeometry(static_cast<int>(gutterWidth()), 0,
        std::max(0, viewport()->width() - static_cast<int>(gutterWidth()) - static_cast<int>(stripeWidth())),
        static_cast<int>(m_lineHeight));
    m_stickyScope->show();
    m_stickyScope->raise();
}
}
