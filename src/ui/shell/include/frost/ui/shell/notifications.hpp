// SPDX-License-Identifier: Apache-2.0
//
// Notifications (PLAN.md row 219, ADR 0035): NotificationCenter keeps the notification log and
// shows each new notification as a balloon in the bottom-right corner of the shell's central
// area; NotificationLogView lists the log in the Notifications tool window.
#pragma once

#include <QDateTime>
#include <QFrame>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

class QListWidget;
class QTimer;

namespace frost::ui {

enum class NotificationKind : std::uint8_t { Info, Warning, Error };

// A link-style button in a balloon and the log; running it closes the balloon.
struct NotificationAction {
    QString label;
    std::function<void()> run;
};

struct Notification {
    QString title;
    QString text;
    NotificationKind kind = NotificationKind::Info;
    // A sticky balloon stays until it is closed; others hide after the center's timeout. Unset:
    // sticky for errors only.
    std::optional<bool> sticky;
    std::vector<NotificationAction> actions;
};

// One log entry.
struct NotificationEntry {
    std::uint64_t id = 0;
    QDateTime time;
    Notification notification;
    bool read = false;
};

class NotificationBalloon;

/*
 * The log and the balloons. notify() runs on the GUI thread; post() may be called from any thread
 * and queues notify() there. The log keeps the newest kMaxEntries entries. Balloons are child
 * widgets of the balloon host (the shell's central widget), stacked upwards from its bottom-right
 * corner, never top-level windows.
 */
class NotificationCenter final : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t kMaxEntries = 500;

    explicit NotificationCenter(QObject* parent = nullptr);
    ~NotificationCenter() override;

    // Logs `notification`, shows its balloon (when a host is set) and returns its id.
    std::uint64_t notify(Notification notification);
    // Thread-safe: queues notify() on the center's thread.
    void post(Notification notification);

    [[nodiscard]] const std::deque<NotificationEntry>& log() const noexcept { return m_log; }
    [[nodiscard]] int unreadCount() const noexcept;
    void markAllRead();
    void clear();

    // The widget balloons stack over (nullptr: log only). Must outlive the center or be reset.
    void setBalloonHost(QWidget* host);
    // How long non-sticky balloons stay (default 10 s).
    void setBalloonTimeout(std::chrono::milliseconds timeout) noexcept { m_timeout = timeout; }
    // The open balloons (on screen once the host is), oldest first.
    [[nodiscard]] std::vector<NotificationBalloon*> balloons() const;
    // Hides every non-sticky balloon now, as their timers would.
    void expireBalloons();

signals:
    // The log or the read state changed.
    void changed();
    void notified(std::uint64_t id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void layoutBalloons();
    void removeBalloon(NotificationBalloon* balloon);

    std::deque<NotificationEntry> m_log;
    std::uint64_t m_nextId = 1;
    QPointer<QWidget> m_host;
    std::vector<QPointer<NotificationBalloon>> m_balloons;
    std::chrono::milliseconds m_timeout{10'000};
};

/*
 * One balloon: kind, title, text, the actions and a close button. Created and positioned by
 * NotificationCenter. GUI thread only.
 */
class NotificationBalloon final : public QFrame {
    Q_OBJECT

public:
    NotificationBalloon(const NotificationEntry& entry, QWidget* host);

    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] bool sticky() const noexcept { return m_sticky; }
    // Starts the hide timer (non-sticky balloons only).
    void armTimer(std::chrono::milliseconds timeout);

signals:
    void dismissed(frost::ui::NotificationBalloon* balloon);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::uint64_t m_id;
    bool m_sticky;
    QTimer* m_timer = nullptr;
};

/*
 * The Notifications tool window's content: the log, newest first, with Clear All. Showing it marks
 * every entry read. GUI thread only; the center must outlive the view.
 */
class NotificationLogView final : public QWidget {
    Q_OBJECT

public:
    explicit NotificationLogView(NotificationCenter& center, QWidget* parent = nullptr);

    [[nodiscard]] QListWidget* list() const noexcept { return m_list; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void rebuild();

    NotificationCenter& m_center;
    QListWidget* m_list;
};

} // namespace frost::ui
