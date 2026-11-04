// SPDX-License-Identifier: Apache-2.0
//
// GitLogToolWindow: commit history table with lane graph, ref badges, commit details and history actions.

#pragma once

#include "frost/vcs/git_repository.hpp"

#include <QPointer>
#include <QString>
#include <QWidget>

#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPoint;
class QPushButton;
class QSplitter;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace frost::ui {

class GitController;

class GitLogToolWindow final : public QWidget {
    Q_OBJECT

public:
    explicit GitLogToolWindow(QWidget* parent = nullptr);
    explicit GitLogToolWindow(GitController* controller, QWidget* parent = nullptr);
    ~GitLogToolWindow() override = default;

    void setController(GitController* controller);
    [[nodiscard]] GitController* controller() const noexcept { return m_controller; }

    void refresh();
    void setCommits(std::vector<vcs::GitCommitEntry> commits);
    [[nodiscard]] const std::vector<vcs::GitCommitEntry>& commits() const noexcept { return m_commits; }

    [[nodiscard]] QLineEdit* searchEdit() const noexcept { return m_searchEdit; }
    [[nodiscard]] QComboBox* branchCombo() const noexcept { return m_branchCombo; }
    [[nodiscard]] QPushButton* refreshButton() const noexcept { return m_refreshButton; }
    [[nodiscard]] QTreeWidget* commitTree() const noexcept { return m_commitTree; }
    [[nodiscard]] QTextEdit* detailsEdit() const noexcept { return m_detailsEdit; }
    [[nodiscard]] QListWidget* filesList() const noexcept { return m_filesList; }

signals:
    void commitSelected(const QString& commitHash);
    void openCommitFileRequested(const QString& commitHash, const QString& relativePath);

private:
    void buildUi();
    void rebuildBranchCombo();
    void populateCommitTable();
    void onCurrentCommitChanged(QTreeWidgetItem* current);
    void showCommitContextMenu(const QPoint& pos);
    void promptNewBranchFrom(const QString& commitHash);
    void promptNewTagAt(const QString& commitHash);
    void confirmAndResetBranch(const QString& commitHash, const QString& mode);

    QPointer<GitController> m_controller;
    std::vector<vcs::GitCommitEntry> m_commits;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_branchCombo = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QSplitter* m_splitter = nullptr;
    QTreeWidget* m_commitTree = nullptr;
    QTextEdit* m_detailsEdit = nullptr;
    QListWidget* m_filesList = nullptr;
    bool m_updatingBranches = false;
};

} // namespace frost::ui
