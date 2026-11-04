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

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

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

    void setUpdateStatus(const QString& status);
    [[nodiscard]] QString updateStatus() const;

    [[nodiscard]] QListWidget* recentList() const noexcept { return m_recent; }
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_search; }
    [[nodiscard]] QListWidget* navigationList() const noexcept { return m_navList; }
    [[nodiscard]] QPushButton* newProjectButton() const noexcept { return m_newProject; }
    [[nodiscard]] QPushButton* openButton() const noexcept { return m_open; }
    [[nodiscard]] QPushButton* cloneRepositoryButton() const noexcept { return m_cloneRepo; }
    [[nodiscard]] QPushButton* openFolderButton() const noexcept { return m_openFolder; }
    [[nodiscard]] QPushButton* checkForUpdatesButton() const noexcept { return m_checkUpdates; }
    [[nodiscard]] QLabel* updateStatusLabel() const noexcept { return m_updateStatus; }

    // Emits openRecent for row `row` (no-op outside the list).
    void activateRow(int row);
    // Emits removeRecent for row `row` (no-op outside the list).
    void removeRow(int row);

signals:
    void newProjectRequested();
    void openRequested();
    void cloneRepositoryRequested();
    void openFolderRequested();
    void openRecent(const QString& path);
    void removeRecent(const QString& path);
    void quickSwitchThemeRequested();
    void settingsRequested();
    void checkForUpdatesRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showContextMenu(const QPoint& position);
    void filterRecentProjects(const QString& query);

    QListWidget* m_navList = nullptr;
    QStackedWidget* m_pages = nullptr;
    QLineEdit* m_search = nullptr;
    QPushButton* m_newProject = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_cloneRepo = nullptr;
    QPushButton* m_openFolder = nullptr;
    QPushButton* m_checkUpdates = nullptr;
    QLabel* m_updateStatus = nullptr;
    QListWidget* m_recent = nullptr;
};

// Standalone compact JetBrains Rider-style Launcher window that opens before the main IDE shell and
// closes when a project or file is opened.
class LauncherWindow final : public QWidget {
    Q_OBJECT

public:
    explicit LauncherWindow(QWidget* parent = nullptr);
    ~LauncherWindow() override;

    [[nodiscard]] WelcomeScreen* welcomeScreen() const noexcept { return m_welcome; }
    void setTransitioningToIde(bool transitioning) noexcept { m_transitioningToIde = transitioning; }

signals:
    void closedByUser();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_titleBar = nullptr;
    WelcomeScreen* m_welcome = nullptr;
    bool m_transitioningToIde = false;
};

} // namespace frost::app
