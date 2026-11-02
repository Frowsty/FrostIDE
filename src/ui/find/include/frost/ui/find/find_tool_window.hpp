// SPDX-License-Identifier: Apache-2.0
//
// FindToolWindow (ADR 0045): the content of the Find tool window (Alt+3), where Open in Find
// Window moves a search's results: a tree of the files with their match counts and, below each,
// its matches with the match highlighted; a toolbar with Rerun, Previous Occurrence, Next
// Occurrence (Ctrl+Alt+Up and Down) and Stop; and the search's status. It shows one search at a
// time (the newest replaces the older one).
/*
 * The window owns its FindSession (setSession). Results keep streaming in while the search runs.
 * Enter or a double-click on a match, and the occurrence steps, open the match through
 * locationActivated(). GUI thread only.
 */
#pragma once

#include "frost/ui/find/find_session.hpp"

#include <QPointer>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <utility>

class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace frost::ui {

class FindToolWindow final : public QWidget {
    Q_OBJECT

public:
    explicit FindToolWindow(QWidget* parent = nullptr);
    ~FindToolWindow() override;
    FindToolWindow(const FindToolWindow&) = delete ("QObject instances have identity and cannot be copied");
    FindToolWindow& operator=(const FindToolWindow&) = delete ("QObject instances have identity and cannot be copied");

    // Shows `session` (which may still be running) and takes ownership; the previous session is
    // deleted. nullptr clears the window.
    void setSession(FindSession* session);
    [[nodiscard]] FindSession* session() const noexcept { return m_session; }

    // Selects the next (`delta` 1) or previous (-1) match in tree order, wrapping at the ends, and
    // opens it. False when there is no match.
    bool stepOccurrence(int delta);
    [[nodiscard]] bool hasOccurrences() const;
    // The selected match as (file, match) indices, or nullopt (nothing or a file row selected).
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> currentMatch() const;
    // Opens the selected match. False without one.
    bool activateCurrent();

    // The widgets, for tests.
    [[nodiscard]] QTreeWidget* tree() const noexcept { return m_tree; }
    [[nodiscard]] QLabel* statusLabel() const noexcept { return m_status; }
    [[nodiscard]] QToolButton* rerunButton() const noexcept { return m_rerun; }
    [[nodiscard]] QToolButton* previousButton() const noexcept { return m_previous; }
    [[nodiscard]] QToolButton* nextButton() const noexcept { return m_next; }
    [[nodiscard]] QToolButton* stopButton() const noexcept { return m_stop; }

signals:
    void locationActivated(const frost::ui::FindLocation& location);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void onFilesAdded(int first, int count);
    void updateStatus();

    QPointer<FindSession> m_session;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_status = nullptr;
    QToolButton* m_rerun = nullptr;
    QToolButton* m_previous = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_stop = nullptr;
};

} // namespace frost::ui
