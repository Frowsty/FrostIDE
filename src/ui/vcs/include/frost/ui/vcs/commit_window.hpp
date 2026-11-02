// SPDX-License-Identifier: Apache-2.0
//
// The Commit tool window (M5, ADR 0048; Alt+0, tool window "commit"): CommitChangesModel, the
// checkable tree of changed files in two groups (Changes, Unversioned Files), and CommitToolWindow,
// the tree above a commit message editor with its history, Amend, and the Commit, Commit and Push
// and Rollback buttons.
/*
 * Selection. Each file has a checkbox; tracked changes start checked and unversioned files
 * unchecked (IntelliJ's defaults), and every later refresh keeps what the user chose for the paths
 * that are still listed. A group's checkbox checks or clears its files. Commit commits the checked
 * files only (GitRepository::commit: `git commit --only`, so nothing else of the index changes).
 *
 * Commit. Commit needs a message, and a checked file unless it amends. While it runs the
 * buttons are disabled. On success the message clears and Amend unchecks; on failure (a pre-commit
 * hook, nothing to commit) the selection and the message stay, and the error, with the hook's
 * output, shows under the buttons. Amend loads the previous commit's message into an empty editor
 * (or one still holding the text Amend loaded) and restores what was typed when it is unchecked.
 *
 * Rollback asks first (setConfirmHandler replaces the dialog in tests). A double click on a file,
 * or Show Diff (Ctrl+D), emits diffRequested for the file under the cursor.
 *
 * GUI thread only; the controller must outlive the window.
 */
#pragma once

#include "frost/vcs/git_service.hpp"
#include "frost/vcs/git_status.hpp"

#include <QPointer>
#include <QStandardItemModel>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QTreeView;

namespace frost::ui {

class GitController;

class CommitChangesModel final : public QStandardItemModel {
    Q_OBJECT

public:
    enum Role : int {
        PathRole = Qt::UserRole + 1, // QString: the absolute path (files only).
        StatusRole,                  // int: vcs::VcsFileStatus (files only).
        GroupRole,                   // int: 0 Changes, 1 Unversioned Files (group rows only).
    };

    explicit CommitChangesModel(QObject* parent = nullptr);

    // Rebuilds the groups from `status` (the work tree at `workTree`), keeping the user's choices. O(entries).
    void setStatus(const std::string& workTree, const vcs::GitStatus& status);
    // The checked files' absolute paths, Changes first, each group in its row order.
    [[nodiscard]] std::vector<std::string> checkedPaths() const;
    // Every listed file's absolute path.
    [[nodiscard]] std::vector<std::string> allPaths() const;
    // Checks or clears one file (no-op for a path that is not listed).
    void setChecked(const std::string& absolutePath, bool checked);
    [[nodiscard]] bool isChecked(const std::string& absolutePath) const;
    // The group rows (nullptr when the group is empty and hidden).
    [[nodiscard]] QStandardItem* changesGroup() const noexcept { return m_changes; }
    [[nodiscard]] QStandardItem* unversionedGroup() const noexcept { return m_unversioned; }
    // The item of a file, or nullptr.
    [[nodiscard]] QStandardItem* itemFor(const std::string& absolutePath) const;

private:
    void onItemChanged(QStandardItem* item);
    void updateGroupCheck(QStandardItem* group);

    QStandardItem* m_changes = nullptr;
    QStandardItem* m_unversioned = nullptr;
    std::set<std::string> m_checked;   // Paths the user (or the defaults) checked.
    std::set<std::string> m_unchecked; // Paths the user cleared.
    bool m_updating = false;
};

class CommitToolWindow final : public QWidget {
    Q_OBJECT

public:
    // Asks the user a yes/no question; true to go on.
    using ConfirmHandler = std::function<bool(const QString& title, const QString& question)>;

    explicit CommitToolWindow(GitController& controller, QWidget* parent = nullptr);
    ~CommitToolWindow() override;

    [[nodiscard]] CommitChangesModel& model() noexcept { return *m_model; }
    [[nodiscard]] QTreeView* tree() const noexcept { return m_tree; }
    [[nodiscard]] QPlainTextEdit* messageEditor() const noexcept { return m_message; }
    [[nodiscard]] QCheckBox* amendCheck() const noexcept { return m_amend; }
    [[nodiscard]] QPushButton* commitButton() const noexcept { return m_commit; }
    [[nodiscard]] QPushButton* commitAndPushButton() const noexcept { return m_commitAndPush; }
    [[nodiscard]] QPushButton* rollbackButton() const noexcept { return m_rollback; }
    [[nodiscard]] QToolButton* historyButton() const noexcept { return m_historyButton; }
    // The result line under the buttons (an error, or "Committed ...").
    [[nodiscard]] QLabel* resultLabel() const noexcept { return m_result; }
    [[nodiscard]] QString message() const;
    void setMessage(const QString& message);
    // The message history the history button offers (the controller's).
    [[nodiscard]] QStringList messageHistory() const;
    // Puts history entry `index` into the editor.
    void useHistoryMessage(int index);

    void setConfirmHandler(ConfirmHandler handler) { m_confirm = std::move(handler); }

    // The actions behind the buttons; each returns false when it could not start.
    bool commit();
    bool commitAndPush();
    // Rolls back the checked files after the confirmation.
    bool rollbackSelected();
    // Emits diffRequested for the current file; false without one.
    bool showDiff();
    // The absolute path of the file under the tree's cursor, or "".
    [[nodiscard]] std::string currentPath() const;

signals:
    // Show the diff of this file (HEAD against the working copy).
    void diffRequested(const QString& absolutePath);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    bool startCommit(bool push);
    void refreshFromController();
    void updateButtons();
    void onAmendToggled(bool on);
    void onOperationFinished(int kind, bool succeeded, const QString& message);
    void showHistoryMenu();

    QPointer<GitController> m_controller;
    CommitChangesModel* m_model;
    QTreeView* m_tree;
    QPlainTextEdit* m_message;
    QToolButton* m_historyButton;
    QCheckBox* m_amend;
    QPushButton* m_commit;
    QPushButton* m_commitAndPush;
    QPushButton* m_rollback;
    QLabel* m_result;
    ConfirmHandler m_confirm;
    bool m_committing = false;
    QString m_typedBeforeAmend;
    QString m_amendMessage; // What Amend loaded, to tell it from the user's own text.
};

} // namespace frost::ui
