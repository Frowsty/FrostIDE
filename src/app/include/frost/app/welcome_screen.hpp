// SPDX-License-Identifier: Apache-2.0
//
// WelcomeScreen: what the editor area shows while no project is open (W1, ADR 0044), Rider's
// Welcome screen in small: the product name, Open (a solution or a file) and Open Folder buttons,
// and the recent projects with their paths, each opened by a click or Enter and removed from the
// list by Delete or the context menu's Remove from Recent Projects.
//
// GUI thread only. The screen holds no state besides the list it was given; the workbench owns
// the recent-projects setting and calls setRecentProjects after every change.
#pragma once

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace frost::app {

class WelcomeScreen final : public QWidget {
    Q_OBJECT

public:
    explicit WelcomeScreen(QWidget* parent = nullptr);
    ~WelcomeScreen() override;

    // Shows `paths` (absolute, newest first); each row shows the file or folder name and the path.
    void setRecentProjects(const std::vector<std::string>& paths);
    // The paths shown, in order.
    [[nodiscard]] std::vector<std::string> recentProjects() const;

    [[nodiscard]] QListWidget* recentList() const noexcept { return m_recent; }
    [[nodiscard]] QPushButton* newProjectButton() const noexcept { return m_newProject; }
    [[nodiscard]] QPushButton* openButton() const noexcept { return m_open; }
    [[nodiscard]] QPushButton* openFolderButton() const noexcept { return m_openFolder; }

    // Emits openRecent for row `row` (no-op outside the list).
    void activateRow(int row);
    // Emits removeRecent for row `row` (no-op outside the list).
    void removeRow(int row);

signals:
    void newProjectRequested();
    void openRequested();
    void openFolderRequested();
    void openRecent(const QString& path);
    void removeRecent(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showContextMenu(const QPoint& position);

    QPushButton* m_newProject = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_openFolder = nullptr;
    QListWidget* m_recent = nullptr;
};

} // namespace frost::app
