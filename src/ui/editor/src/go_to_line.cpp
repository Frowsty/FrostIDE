// SPDX-License-Identifier: Apache-2.0
//
// GoToLinePopup (frost/ui/editor/go_to_line.hpp): a framed label, field and error line with Enter
// and Escape handling.

#include "frost/ui/editor/go_to_line.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace frost::ui {

GoToLinePopup::GoToLinePopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("frostGoToLine"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);
    auto* label = new QLabel(QStringLiteral("[Line] [:column]:"), this);
    m_field = new QLineEdit(this);
    m_field->setMinimumWidth(220);
    m_error = new QLabel(this);
    m_error->setVisible(false);
    layout->addWidget(label);
    layout->addWidget(m_field);
    layout->addWidget(m_error);
    m_field->installEventFilter(this);
}

void GoToLinePopup::open(const QString& text) {
    m_error->clear();
    m_error->setVisible(false);
    m_field->setText(text);
    m_field->selectAll();
    adjustSize();
    show();
    raise();
    m_field->setFocus(Qt::ShortcutFocusReason);
}

void GoToLinePopup::showError(const QString& message) {
    m_error->setText(message);
    m_error->setVisible(true);
    adjustSize();
}

QString GoToLinePopup::errorText() const { return m_error->isVisible() ? m_error->text() : QString(); }

bool GoToLinePopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_field && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
            if (event->type() == QEvent::ShortcutOverride) {
                keyEvent->accept();
                return true;
            }
            if (key == Qt::Key_Escape) {
                emit cancelled();
            } else {
                emit accepted(m_field->text());
            }
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace frost::ui
