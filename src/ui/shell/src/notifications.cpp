// SPDX-License-Identifier: Apache-2.0
//
// NotificationCenter, NotificationBalloon and NotificationLogView (frost/ui/shell/notifications.hpp).

#include "frost/ui/shell/notifications.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui {

using shell_detail::text;

namespace {

constexpr int kBalloonWidth = 340;
constexpr int kBalloonMargin = 12;

std::string_view kindIcon(NotificationKind kind) noexcept {
    switch (kind) {
    case NotificationKind::Info: return icons::kStatusInfo;
    case NotificationKind::Warning: return icons::kStatusWarning;
    case NotificationKind::Error: return icons::kStatusError;
    }
    return icons::kStatusInfo;
}

QString kindName(NotificationKind kind) {
    switch (kind) {
    case NotificationKind::Info: return text("Information");
    case NotificationKind::Warning: return text("Warning");
    case NotificationKind::Error: return text("Error");
    }
    return {};
}

} // namespace

NotificationCenter::NotificationCenter(QObject* parent) : QObject(parent) {
    setObjectName(QStringLiteral("notificationCenter"));
}

NotificationCenter::~NotificationCenter() { setBalloonHost(nullptr); }

std::uint64_t NotificationCenter::notify(Notification notification) {
    NotificationEntry entry;
    entry.id = m_nextId++;
    entry.time = QDateTime::currentDateTime();
    entry.notification = std::move(notification);
    m_log.push_back(std::move(entry));
    while (m_log.size() > kMaxEntries) {
        m_log.pop_front();
    }
    const NotificationEntry& added = m_log.back();
    if (m_host != nullptr) {
        auto* balloon = new NotificationBalloon(added, m_host);
        connect(balloon, &NotificationBalloon::dismissed, this, &NotificationCenter::removeBalloon);
        m_balloons.emplace_back(balloon);
        balloon->show();
        balloon->raise();
        if (!balloon->sticky()) {
            balloon->armTimer(m_timeout);
        }
        layoutBalloons();
    }
    emit changed();
    emit notified(added.id);
    return added.id;
}

void NotificationCenter::post(Notification notification) {
    QMetaObject::invokeMethod(
        this, [this, moved = std::move(notification)]() mutable { static_cast<void>(notify(std::move(moved))); },
        Qt::QueuedConnection);
}

int NotificationCenter::unreadCount() const noexcept {
    return static_cast<int>(std::ranges::count_if(m_log, [](const NotificationEntry& e) { return !e.read; }));
}

void NotificationCenter::markAllRead() {
    bool changedAny = false;
    for (NotificationEntry& entry : m_log) {
        changedAny = changedAny || !entry.read;
        entry.read = true;
    }
    if (changedAny) {
        emit changed();
    }
}

void NotificationCenter::clear() {
    m_log.clear();
    emit changed();
}

void NotificationCenter::setBalloonHost(QWidget* host) {
    if (m_host != nullptr) {
        m_host->removeEventFilter(this);
    }
    for (const QPointer<NotificationBalloon>& balloon : m_balloons) {
        delete balloon.data();
    }
    m_balloons.clear();
    m_host = host;
    if (host != nullptr) {
        host->installEventFilter(this);
    }
}

std::vector<NotificationBalloon*> NotificationCenter::balloons() const {
    std::vector<NotificationBalloon*> shown;
    for (const QPointer<NotificationBalloon>& balloon : m_balloons) {
        if (balloon != nullptr && !balloon->isHidden()) {
            shown.push_back(balloon.data());
        }
    }
    return shown;
}

void NotificationCenter::expireBalloons() {
    const std::vector<QPointer<NotificationBalloon>> current = m_balloons;
    for (const QPointer<NotificationBalloon>& balloon : current) {
        if (balloon != nullptr && !balloon->sticky()) {
            removeBalloon(balloon.data());
        }
    }
}

bool NotificationCenter::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_host && event->type() == QEvent::Resize) {
        layoutBalloons();
    }
    return QObject::eventFilter(watched, event);
}

void NotificationCenter::layoutBalloons() {
    if (m_host == nullptr) {
        return;
    }
    // Newest at the bottom, older ones above it.
    int bottom = m_host->height() - kBalloonMargin;
    for (auto it = m_balloons.rbegin(); it != m_balloons.rend(); ++it) {
        NotificationBalloon* balloon = it->data();
        if (balloon == nullptr) {
            continue;
        }
        balloon->adjustSize();
        const int width = std::min(kBalloonWidth, std::max(120, m_host->width() - 2 * kBalloonMargin));
        const int height =
            balloon->heightForWidth(width) > 0 ? balloon->heightForWidth(width) : balloon->sizeHint().height();
        balloon->setGeometry(m_host->width() - kBalloonMargin - width, bottom - height, width, height);
        bottom -= height + kBalloonMargin / 2;
    }
}

void NotificationCenter::removeBalloon(NotificationBalloon* balloon) {
    std::erase_if(m_balloons,
                  [balloon](const QPointer<NotificationBalloon>& b) { return b == nullptr || b == balloon; });
    if (balloon != nullptr) {
        balloon->hide();
        balloon->deleteLater();
    }
    layoutBalloons();
}

NotificationBalloon::NotificationBalloon(const NotificationEntry& entry, QWidget* host)
    : QFrame(host), m_id(entry.id),
      m_sticky(entry.notification.sticky.value_or(entry.notification.kind == NotificationKind::Error)) {
    setObjectName(QStringLiteral("notificationBalloon"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
    const Notification& notification = entry.notification;

    auto* icon = new QLabel(this);
    const QIcon kind = shell_detail::themedIcon(kindIcon(notification.kind));
    if (!kind.isNull()) {
        icon->setPixmap(kind.pixmap(16, 16));
    }
    icon->setToolTip(kindName(notification.kind));
    auto* title = new QLabel(notification.title, this);
    title->setObjectName(QStringLiteral("balloonTitle"));
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    title->setTextFormat(Qt::PlainText);
    auto* close = new QToolButton(this);
    close->setObjectName(QStringLiteral("balloonClose"));
    close->setAutoRaise(true);
    close->setToolTip(text("Close"));
    const QIcon closeIcon = shell_detail::themedIcon(icons::kActionsClose);
    if (closeIcon.isNull()) {
        close->setText(QStringLiteral("×"));
    } else {
        close->setIcon(closeIcon);
    }
    connect(close, &QToolButton::clicked, this, [this] { emit dismissed(this); });

    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    header->addWidget(icon);
    header->addWidget(title, 1);
    header->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);
    layout->addLayout(header);
    if (!notification.text.isEmpty()) {
        auto* body = new QLabel(notification.text, this);
        body->setObjectName(QStringLiteral("balloonText"));
        body->setTextFormat(Qt::PlainText);
        body->setWordWrap(true);
        layout->addWidget(body);
    }
    if (!notification.actions.empty()) {
        auto* actions = new QHBoxLayout();
        actions->setSpacing(10);
        for (const NotificationAction& action : notification.actions) {
            auto* button = new QPushButton(escapeMnemonics(action.label), this);
            button->setObjectName(QStringLiteral("balloonAction"));
            button->setFlat(true);
            button->setCursor(Qt::PointingHandCursor);
            connect(button, &QPushButton::clicked, this, [this, run = action.run] {
                if (run) {
                    run();
                }
                emit dismissed(this);
            });
            actions->addWidget(button);
        }
        actions->addStretch(1);
        layout->addLayout(actions);
    }
}

void NotificationBalloon::armTimer(std::chrono::milliseconds timeout) {
    if (m_timer == nullptr) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] { emit dismissed(this); });
    }
    m_timer->start(timeout);
}

void NotificationBalloon::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const FrostStyle* style = FrostStyle::current();
    const QColor background =
        style != nullptr ? style->color(ColorKey::PopupBackground) : palette().color(QPalette::ToolTipBase);
    const QColor border = style != nullptr ? style->color(ColorKey::PopupBorderColor) : palette().color(QPalette::Mid);
    const int radius = style != nullptr ? style->metricPx(MetricKey::IslandArc) : 6;
    painter.setPen(border);
    painter.setBrush(background);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

NotificationLogView::NotificationLogView(NotificationCenter& center, QWidget* parent)
    : QWidget(parent), m_center(center), m_list(new QListWidget(this)) {
    setObjectName(QStringLiteral("notificationLog"));
    m_list->setObjectName(QStringLiteral("notificationLogList"));
    m_list->setWordWrap(true);
    m_list->setFrameShape(QFrame::NoFrame);
    auto* clear = new QToolButton(this);
    clear->setObjectName(QStringLiteral("notificationLogClear"));
    clear->setText(text("Clear All"));
    clear->setAutoRaise(true);
    connect(clear, &QToolButton::clicked, &m_center, &NotificationCenter::clear);
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 2, 6, 2);
    bar->addStretch(1);
    bar->addWidget(clear);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_list, 1);
    connect(&m_center, &NotificationCenter::changed, this, &NotificationLogView::rebuild);
    rebuild();
}

void NotificationLogView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_center.markAllRead();
}

void NotificationLogView::rebuild() {
    m_list->clear();
    const auto& log = m_center.log();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        const Notification& n = it->notification;
        QString line = it->time.toString(QStringLiteral("HH:mm")) + QStringLiteral("  ") + n.title;
        if (!n.text.isEmpty()) {
            line += QStringLiteral("\n") + n.text;
        }
        auto* item = new QListWidgetItem(shell_detail::themedIcon(kindIcon(n.kind)), line, m_list);
        QFont font = item->font();
        font.setBold(!it->read);
        item->setFont(font);
    }
}

} // namespace frost::ui
