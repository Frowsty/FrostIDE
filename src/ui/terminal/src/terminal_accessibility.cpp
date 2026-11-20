// SPDX-License-Identifier: Apache-2.0
// Read-only terminal text interface over the visible cell grid, including wide and combining characters.
#include "terminal_accessibility.hpp"
#include "frost/ui/terminal/terminal_view.hpp"
#include <QAccessible>
#include <QAccessibleWidget>
#include <algorithm>
#include <cmath>
#include <vector>

namespace frost::ui {
class AccessibleTerminal final : public QAccessibleWidget, public QAccessibleTextInterface {
    struct Snapshot {
        QString text;
        std::vector<term::ScreenPos> positions;
        int cursor = -1;
    };
public:
    explicit AccessibleTerminal(TerminalView* view) : QAccessibleWidget(view, QAccessible::Terminal), m_view(view) {}
    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QAccessible::State state() const override {
        auto result = QAccessibleWidget::state();
        result.readOnly = true; result.multiLine = true; result.selectableText = true;
        return result;
    }
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Name) return m_view->title().isEmpty() ? QStringLiteral("Terminal output") : m_view->title();
        if (type == QAccessible::Value) return snapshot().text;
        return QAccessibleWidget::text(type);
    }
    int characterCount() const override { return static_cast<int>(snapshot().text.size()); }
    QString text(int start, int end) const override {
        const auto data = snapshot();
        if (end == -1) end = static_cast<int>(data.text.size());
        if (start < 0 || end < start || end > data.text.size()) return {};
        return data.text.mid(start, end - start);
    }
    int cursorPosition() const override { return snapshot().cursor; }
    // A terminal cursor belongs to the program: accessibility never sends escape sequences to move it.
    void setCursorPosition(int) override {}
    int selectionCount() const override { int start = -1, end = -1; selection(0, &start, &end); return start >= 0 && end > start ? 1 : 0; }
    void selection(int index, int* start, int* end) const override {
        if (start) *start = -1;
        if (end) *end = -1;
        if (index != 0 || !m_view->m_selection) return;
        const auto data = snapshot();
        const auto from = std::min(m_view->m_selection->anchor, m_view->m_selection->end);
        const auto to = std::max(m_view->m_selection->anchor, m_view->m_selection->end);
        const auto first = std::lower_bound(data.positions.begin(), data.positions.end(), from);
        const auto last = std::upper_bound(data.positions.begin(), data.positions.end(), to);
        if (first == data.positions.end() || first == last) return;
        if (start) *start = static_cast<int>(first - data.positions.begin());
        if (end) *end = static_cast<int>(last - data.positions.begin());
    }
    void setSelection(int index, int start, int end) override { if (index == 0) addSelection(start, end); }
    void addSelection(int start, int end) override {
        const auto data = snapshot();
        if (start < 0 || end <= start || end > data.text.size()) return;
        m_view->select(data.positions[static_cast<std::size_t>(start)], data.positions[static_cast<std::size_t>(end - 1)]);
    }
    void removeSelection(int index) override { if (index == 0) m_view->clearSelection(); }
    QRect characterRect(int offset) const override {
        const auto data = snapshot();
        if (offset < 0 || static_cast<std::size_t>(offset) >= data.positions.size()) return {};
        const auto pos = data.positions[static_cast<std::size_t>(offset)];
        const auto cell = m_view->session()->read([pos](const term::VtScreen& screen) { return screen.cell(pos.row, pos.column); });
        const auto size = m_view->cellSize();
        const QPoint origin = m_view->mapToGlobal(QPoint(static_cast<int>(std::floor(pos.column * size.width())), static_cast<int>(std::floor((pos.row + m_view->scrollOffset()) * size.height()))));
        return QRect(origin, QSize(static_cast<int>(std::ceil(std::max(1, static_cast<int>(cell.width)) * size.width())), static_cast<int>(std::ceil(size.height()))));
    }
    int offsetAtPoint(const QPoint& point) const override {
        const auto local = m_view->mapFromGlobal(point);
        if (!m_view->rect().contains(local)) return -1;
        const auto size = m_view->cellSize();
        const term::ScreenPos pos{static_cast<int>(local.y() / size.height()) - m_view->scrollOffset(), static_cast<int>(local.x() / size.width())};
        const auto data = snapshot();
        auto it = std::lower_bound(data.positions.begin(), data.positions.end(), pos);
        // A wide character's second cell maps to that same character.
        if (it != data.positions.begin() && (it == data.positions.end() || *it != pos)) --it;
        if (it == data.positions.end() || it->row != pos.row) return -1;
        while (it != data.positions.begin() && *(it - 1) == *it) --it;
        return static_cast<int>(it - data.positions.begin());
    }
    QString attributes(int offset, int* start, int* end) const override {
        const int count = characterCount();
        const bool valid = offset >= 0 && offset <= count;
        if (start) *start = valid ? 0 : -1;
        if (end) *end = valid ? count : -1;
        return {};
    }
    void scrollToSubstring(int start, int end) override {
        const auto data = snapshot();
        if (start < 0 || end < start || static_cast<std::size_t>(start) >= data.positions.size()) return;
        m_view->setScrollOffset(std::max(0, -data.positions[static_cast<std::size_t>(start)].row));
    }
private:
    Snapshot snapshot() const {
        if (!m_view->session()) return {};
        const int offset = m_view->scrollOffset();
        return m_view->session()->read([offset](const term::VtScreen& screen) {
            Snapshot result;
            const auto cursor = screen.cursor();
            for (int visual = 0; visual < screen.rows(); ++visual) {
                const int row = visual - offset;
                const auto cells = screen.line(row);
                QString line;
                std::vector<term::ScreenPos> positions;
                for (std::size_t column = 0; column < cells.size(); ++column) {
                    const auto& cell = cells[column];
                    if (!cell.width) continue;
                    const auto value = cell.text.empty() ? QStringLiteral(" ") : QString::fromUtf8(cell.text.data(), static_cast<qsizetype>(cell.text.size()));
                    line += value;
                    positions.insert(positions.end(), static_cast<std::size_t>(value.size()), term::ScreenPos{row, static_cast<int>(column)});
                }
                while (line.endsWith(QLatin1Char(' '))) { line.chop(1); positions.pop_back(); }
                if (row == cursor.row) {
                    const auto at = std::lower_bound(positions.begin(), positions.end(), term::ScreenPos{row, cursor.column});
                    result.cursor = static_cast<int>(result.text.size() + (at - positions.begin()));
                }
                result.text += line;
                result.positions.insert(result.positions.end(), positions.begin(), positions.end());
                if (visual + 1 < screen.rows()) {
                    result.text += QLatin1Char('\n');
                    result.positions.push_back({row, screen.columns()});
                }
            }
            return result;
        });
    }
    TerminalView* m_view;
};
namespace {
QAccessibleInterface* terminalFactory(const QString&, QObject* object) {
    if (auto* view = qobject_cast<TerminalView*>(object)) return new AccessibleTerminal(view);
    return nullptr;
}
}
void initializeTerminalAccessibility(TerminalView&) {
    static const bool installed = [] { QAccessible::installFactory(terminalFactory); return true; }();
    (void)installed;
}
void notifyTerminalAccessibility(TerminalView& view, bool outputChanged) {
    if (!QAccessible::isActive()) return;
    if (outputChanged) {
        QAccessibleEvent output(&view, QAccessible::VisibleDataChanged);
        QAccessible::updateAccessibility(&output);
    }
    auto* interface = QAccessible::queryAccessibleInterface(&view);
    if (!interface || !interface->textInterface()) return;
    auto* text = interface->textInterface();
    int start = text->cursorPosition(), end = start;
    if (text->selectionCount()) text->selection(0, &start, &end);
    QAccessibleTextSelectionEvent selection(&view, start, end);
    QAccessible::updateAccessibility(&selection);
}
}
