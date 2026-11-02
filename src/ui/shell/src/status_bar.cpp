// SPDX-License-Identifier: Apache-2.0
//
// ShellStatusBar (frost/ui/shell/status_bar.hpp).

#include "frost/ui/shell/status_bar.hpp"

#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/notifications.hpp"

#include "shell_util.hpp"
#include <QStatusBar>
#include <QToolButton>

namespace frost::ui {

using shell_detail::text;

namespace {

QToolButton* statusButton(QStatusBar& bar, const char* objectName, const QString& tooltip) {
    auto* button = new QToolButton(&bar);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setToolTip(tooltip);
    bar.addPermanentWidget(button);
    return button;
}

} // namespace

ShellStatusBar::ShellStatusBar(QStatusBar& bar, BackgroundTaskManager& tasks, NotificationCenter& notifications,
                               QObject* parent)
    : QObject(parent), m_center(notifications), m_processes(new ProcessesIndicator(tasks, &bar)) {
    setObjectName(QStringLiteral("shellStatusBar"));
    bar.addPermanentWidget(m_processes);
    m_caret = statusButton(bar, "statusCaret", text("Go to Line:Column"));
    m_separator = statusButton(bar, "statusLineSeparator", text("Line Separator"));
    m_encoding = statusButton(bar, "statusEncoding", text("File Encoding"));
    m_indent = statusButton(bar, "statusIndent", text("Indent"));
    m_readOnly = statusButton(bar, "statusReadOnly", QString());
    m_notifications = statusButton(bar, "statusNotifications", text("Notifications"));
    m_notifications->setIcon(shell_detail::themedIcon(icons::kToolwindowNotifications));
    connect(m_caret, &QToolButton::clicked, this, &ShellStatusBar::caretClicked);
    connect(m_notifications, &QToolButton::clicked, this, &ShellStatusBar::notificationsClicked);
    connect(&m_center, &NotificationCenter::changed, this, &ShellStatusBar::updateNotifications);
    showEditor(nullptr);
    updateNotifications();
}

void ShellStatusBar::showEditor(const EditorStatus* status) {
    for (QToolButton* button : {m_caret, m_separator, m_encoding, m_indent, m_readOnly}) {
        button->setVisible(status != nullptr);
    }
    if (status == nullptr) {
        return;
    }
    m_caret->setText(caretText(*status));
    m_separator->setText(shell_detail::fromUtf8(status->lineSeparator));
    m_encoding->setText(shell_detail::fromUtf8(status->encoding));
    m_indent->setText(indentText(status->indent));
    m_readOnly->setText(status->readOnly ? text("Read-only") : text("Writable"));
    m_readOnly->setToolTip(status->readOnly ? text("The file is read-only") : text("The file is writable"));
}

QString ShellStatusBar::caretText(const EditorStatus& status) {
    const QString position = QStringLiteral("%1:%2").arg(status.line).arg(status.column);
    if (status.selectedCharacters <= 0) {
        return position;
    }
    return text("%1 (%2 chars)").arg(position).arg(status.selectedCharacters);
}

QString ShellStatusBar::indentText(const IndentStatus& indent) {
    if (indent.useTabs) {
        return text("Tab");
    }
    return text("%1 spaces").arg(indent.size);
}

std::vector<QWidget*> ShellStatusBar::focusOrder() const {
    std::vector<QWidget*> order;
    for (QToolButton* button : {m_caret, m_separator, m_encoding, m_indent, m_readOnly, m_notifications}) {
        if (button->isVisible()) {
            order.push_back(button);
        }
    }
    return order;
}

void ShellStatusBar::updateNotifications() {
    const int unread = m_center.unreadCount();
    m_notifications->setText(unread > 0 ? QString::number(unread) : QString());
    if (m_notifications->icon().isNull() && unread == 0) {
        m_notifications->setText(text("Notifications"));
    }
}

} // namespace frost::ui
