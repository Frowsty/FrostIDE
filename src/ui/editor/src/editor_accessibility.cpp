// SPDX-License-Identifier: Apache-2.0
// Qt text accessibility over actual editor snapshots; offsets are UTF-16, edits retain undo/read-only rules.
#include "editor_accessibility.hpp"

#include "frost/ui/editor/editor_view.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QScrollBar>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <utility>

namespace frost::ui {
namespace {
int units(const text::Snapshot& snapshot, ByteOffset offset) {
    return static_cast<int>(std::min<std::uint64_t>(snapshot.utf16At(offset).value(), std::numeric_limits<int>::max()));
}
class AccessibleEditor final : public QAccessibleWidget,
                               public QAccessibleTextInterface,
                               public QAccessibleEditableTextInterface {
public:
    explicit AccessibleEditor(EditorView* view) : QAccessibleWidget(view, QAccessible::EditableText), m_view(view) {}
    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface)
            return static_cast<QAccessibleTextInterface*>(this);
        if (type == QAccessible::EditableTextInterface)
            return m_view->isReadOnly() ? nullptr : static_cast<QAccessibleEditableTextInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QAccessible::State state() const override {
        auto result = QAccessibleWidget::state();
        result.editable = !m_view->isReadOnly();
        result.readOnly = m_view->isReadOnly();
        result.selectableText = true;
        result.multiLine = true;
        return result;
    }
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Name)
            return m_view->path().empty() ? QStringLiteral("Text editor") : QString::fromStdString(m_view->path());
        if (type == QAccessible::Value)
            return text(0, characterCount());
        if (type == QAccessible::Description) {
            QStringList messages;
            const auto caret = m_view->editor().state().primary().head;
            for (const auto& diagnostic : m_view->diagnostics()) {
                if (diagnostic.range.begin <= caret &&
                    (caret < diagnostic.range.end ||
                     (diagnostic.range.begin == diagnostic.range.end && caret == diagnostic.range.begin)))
                    messages.append(diagnosticText(diagnostic));
            }
            return messages.join(QLatin1Char('\n'));
        }
        return QAccessibleWidget::text(type);
    }
    QStringList actionNames() const override {
        auto actions = QAccessibleWidget::actionNames();
        actions.append(QStringLiteral("nextDiagnostic"));
        actions.append(QStringLiteral("previousDiagnostic"));
        return actions;
    }
    QString localizedActionName(const QString& name) const override {
        if (name == QStringLiteral("nextDiagnostic"))
            return QStringLiteral("Next diagnostic");
        if (name == QStringLiteral("previousDiagnostic"))
            return QStringLiteral("Previous diagnostic");
        return QAccessibleWidget::localizedActionName(name);
    }
    void doAction(const QString& name) override {
        if (name == QStringLiteral("nextDiagnostic")) {
            (void)m_view->gotoNextProblem(true);
            return;
        }
        if (name == QStringLiteral("previousDiagnostic")) {
            (void)m_view->gotoNextProblem(false);
            return;
        }
        QAccessibleWidget::doAction(name);
    }
    QStringList keyBindingsForAction(const QString& name) const override {
        // Shortcuts are customizable; advertise no binding rather than a stale default.
        if (name == QStringLiteral("nextDiagnostic") || name == QStringLiteral("previousDiagnostic"))
            return {};
        return QAccessibleWidget::keyBindingsForAction(name);
    }
    int characterCount() const override { return units(snapshot(), snapshot().size()); }
    int cursorPosition() const override { return units(snapshot(), m_view->editor().state().primary().head); }
    void setCursorPosition(int position) override {
        if (position < 0 || position > characterCount())
            return;
        m_view->setState(text::EditorState::caretAt(byte(position)));
    }
    QString text(int start, int end) const override {
        const int count = characterCount();
        if (end == -1)
            end = count;
        if (start < 0 || end < start || end > count)
            return {};
        const auto beginByte = byte(start);
        // Include a complete surrogate pair at either edge, then slice by UTF-16 units.
        const auto endByte = byte(std::min(count, end + (end < count ? 1 : 0)));
        const auto utf8 = snapshot().text({beginByte, endByte});
        return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()))
            .mid(start - units(snapshot(), beginByte), end - start);
    }
    int selectionCount() const override { return static_cast<int>(selected().size()); }
    void selection(int index, int* start, int* end) const override {
        if (start)
            *start = -1;
        if (end)
            *end = -1;
        const auto ranges = selected();
        if (index < 0 || static_cast<std::size_t>(index) >= ranges.size())
            return;
        const auto range = ranges[static_cast<std::size_t>(index)].range();
        if (start)
            *start = units(snapshot(), range.begin);
        if (end)
            *end = units(snapshot(), range.end);
    }
    void addSelection(int start, int end) override {
        if (!validRange(start, end))
            return;
        auto ranges = selected();
        ranges.push_back({byte(start), byte(end)});
        setSelections(ranges);
    }
    void setSelection(int index, int start, int end) override {
        if (!validRange(start, end))
            return;
        auto ranges = selected();
        if (index < 0 || static_cast<std::size_t>(index) >= ranges.size())
            return;
        ranges[static_cast<std::size_t>(index)] = {byte(start), byte(end)};
        setSelections(ranges);
    }
    void removeSelection(int index) override {
        auto ranges = selected();
        if (index < 0 || static_cast<std::size_t>(index) >= ranges.size())
            return;
        const auto caret = ranges[static_cast<std::size_t>(index)].head;
        ranges.erase(ranges.begin() + index);
        if (ranges.empty())
            m_view->setState(text::EditorState::caretAt(caret));
        else
            setSelections(ranges);
    }
    QRect characterRect(int offset) const override {
        if (offset < 0 || offset > characterCount())
            return {};
        auto result = m_view->globalCaretRect(byte(offset));
        int next = std::min(characterCount(), offset + (offset < characterCount() ? 1 : 0));
        if (next < characterCount() && byte(next) == byte(offset))
            ++next;
        const auto nextRect = m_view->globalCaretRect(byte(next));
        if (nextRect.top() == result.top())
            result.setWidth(std::max(1, nextRect.left() - result.left()));
        return result;
    }
    int offsetAtPoint(const QPoint& point) const override {
        if (!m_view->viewport()->rect().contains(m_view->viewport()->mapFromGlobal(point)))
            return -1;
        return units(snapshot(), m_view->offsetAt(m_view->viewport()->mapFromGlobal(point)));
    }
    QString attributes(int offset, int* start, int* end) const override {
        if (start)
            *start = -1;
        if (end)
            *end = -1;
        if (offset < 0 || offset > characterCount())
            return {};
        int begin = 0, finish = characterCount();
        bool invalid = false;
        for (const auto& diagnostic : m_view->diagnostics()) {
            const int from = units(snapshot(), diagnostic.range.begin);
            const int to = units(snapshot(), diagnostic.range.end);
            // Every boundary splits a run, including overlapping diagnostics. Point diagnostics
            // have no text run and are exposed by the caret description instead.
            if (from == to)
                continue;
            for (const int boundary : {from, to}) {
                if (boundary <= offset)
                    begin = std::max(begin, boundary);
                else
                    finish = std::min(finish, boundary);
            }
            invalid = invalid || (from <= offset && offset < to && diagnostic.severity == InsightSeverity::Error);
        }
        if (start)
            *start = begin;
        if (end)
            *end = finish;
        return invalid ? QStringLiteral("invalid:true;") : QString{};
    }
    void scrollToSubstring(int start, int end) override {
        if (!validRange(start, end))
            return;
        const auto offset = byte(start);
        m_view->revealFoldAt(offset);
        m_view->verticalScrollBar()->setValue(
            static_cast<int>(m_view->editor().display().toDisplayPoint(offset).row.value()));
        const auto caret = m_view->globalCaretRect(offset);
        const auto local = m_view->viewport()->mapFromGlobal(caret.topLeft());
        const int overflow =
            local.x() < 0 ? local.x() : std::max(0, local.x() - m_view->viewport()->width() + caret.width());
        m_view->horizontalScrollBar()->setValue(m_view->horizontalScrollBar()->value() + overflow);
    }
    void deleteText(int start, int end) override { replaceText(start, end, {}); }
    void insertText(int offset, const QString& value) override { replaceText(offset, offset, value); }
    void replaceText(int start, int end, const QString& value) override {
        if (m_view->isReadOnly() || !validRange(start, end))
            return;
        const text::Selection range{byte(start), byte(end)};
        m_view->setState(text::EditorState::from(text::SelectionSet::fromSelections(std::span(&range, 1), 0)));
        const auto utf8 = value.toUtf8();
        (void)m_view->pasteText(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    }

private:
    static QString diagnosticText(const ActiveDiagnostic& diagnostic) {
        QString severity;
        switch (diagnostic.severity) {
        case InsightSeverity::Error: severity = QStringLiteral("Error"); break;
        case InsightSeverity::Warning: severity = QStringLiteral("Warning"); break;
        case InsightSeverity::WeakWarning: severity = QStringLiteral("Weak warning"); break;
        case InsightSeverity::Info: severity = QStringLiteral("Information"); break;
        }
        return severity + QStringLiteral(": ") + QString::fromStdString(diagnostic.message);
    }
    const text::Snapshot& snapshot() const { return m_view->editor().text(); }
    bool validRange(int start, int end) const { return start >= 0 && end >= start && end <= characterCount(); }
    ByteOffset byte(int offset) const {
        return snapshot().offsetAtUtf16(Utf16Offset{static_cast<std::uint64_t>(offset)}).value();
    }
    std::vector<text::Selection> selected() const {
        std::vector<text::Selection> result;
        for (const auto& range : m_view->editor().state().selections.all())
            if (!range.empty())
                result.push_back(range);
        return result;
    }
    void setSelections(const std::vector<text::Selection>& ranges) {
        m_view->setState(text::EditorState::from(text::SelectionSet::fromSelections(ranges, ranges.size() - 1)));
    }
    EditorView* m_view;
};
QAccessibleInterface* editorFactory(const QString&, QObject* object) {
    if (auto* view = qobject_cast<EditorView*>(object))
        return new AccessibleEditor(view);
    return nullptr;
}
} // namespace
void initializeEditorAccessibility(EditorView& view) {
    static const bool installed = [] {
        QAccessible::installFactory(editorFactory);
        return true;
    }();
    (void)installed;
    QObject::connect(&view, &EditorView::statusChanged, &view, [&view] {
        if (!QAccessible::isActive())
            return;
        const auto range = view.editor().state().primary().range();
        QAccessibleTextSelectionEvent event(&view, units(view.editor().text(), range.begin),
                                            units(view.editor().text(), range.end));
        QAccessible::updateAccessibility(&event);
        QAccessibleEvent description(&view, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&description);
    });
    QObject::connect(&view, &EditorView::documentEdited, &view,
                     [&view, before = view.editor().text()](quint64) mutable {
                         const auto after = view.editor().text();
                         if (QAccessible::isActive()) {
                             // Large replacements invalidate the accessible value without materializing a huge event
                             // payload.
                             if (before.size().value() + after.size().value() <= 131072) {
                                 QAccessibleTextUpdateEvent event(&view, 0, QString::fromStdString(before.text()),
                                                                  QString::fromStdString(after.text()));
                                 QAccessible::updateAccessibility(&event);
                             } else {
                                 QAccessibleEvent event(&view, QAccessible::VisibleDataChanged);
                                 QAccessible::updateAccessibility(&event);
                             }
                         }
                         before = after;
                     });
}
} // namespace frost::ui
