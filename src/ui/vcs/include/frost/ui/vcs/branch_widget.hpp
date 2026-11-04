// SPDX-License-Identifier: Apache-2.0
//
// The current branch in the status bar and the Branches popup (M5, ADR 0048). BranchStatusWidget
// shows the branch name (or the short commit of a detached HEAD) and opens the popup on a click; the
// popup lists New Branch..., the local branches and the remote ones, checks the chosen one out (a
// remote branch through a local tracking branch) or asks for the new branch's name. The checkout
// runs on the service; a work tree that blocks it ends with the refusal message, which the widget
// shows in its tooltip and the controller as a notification.
//
// GUI thread only; the controller must outlive both. The workbench adds the widget to the status
// bar (ShellStatusBar's permanent widgets).
#pragma once

#include "frost/ui/shell/popup.hpp"

#include <QPointer>
#include <QString>
#include <QToolButton>
#include <QVariant>

#include <vector>

class QMenu;

namespace frost::ui {

class GitController;

// The rows of the Branches popup for the controller's branch list: "New Branch..." (data "new:"),
// then local branches (data "local:<name>"), then remote ones ("remote:<name>"). The current branch
// is marked in its detail. Pure apart from the controller's caches.
[[nodiscard]] std::vector<ListPopupItem> branchPopupItems(const GitController& controller);

class BranchStatusWidget final : public QToolButton {
    Q_OBJECT

public:
    explicit BranchStatusWidget(GitController& controller, QWidget* parent = nullptr);
    ~BranchStatusWidget() override;

    // Opens the Branches popup below the widget (the click does this) and returns it.
    ListPopup* openPopup();
    [[nodiscard]] ListPopup* popup() const noexcept { return m_popup; }
    // Builds or opens the hierarchical Branches QMenu with top actions and per-branch submenus.
    [[nodiscard]] QMenu* buildMenu(QWidget* parent = nullptr);
    QMenu* openMenu();
    // Runs the popup row with `data` (as accepting it does): a checkout, or the new-branch prompt.
    void runItem(const QVariant& data);
    // Prompts to stash working tree changes or apply/pop/drop an existing stash.
    void promptStashChanges();
    void promptUnstashChanges();
    void promptRenameBranch(const QString& branch);
    // The open new-branch prompt, or nullptr.
    [[nodiscard]] InputPopup* newBranchPrompt() const noexcept { return m_prompt; }
    // The last refusal or failure of a checkout or new branch ("" after a success).
    [[nodiscard]] const QString& lastError() const noexcept { return m_lastError; }

signals:
    void fetchRequested();
    void pullRequested();
    void pushRequested();
    void stashRequested();
    void unstashRequested();
    void mergeRequested(const QString& branch);
    void rebaseRequested(const QString& branch);
    void renameBranchRequested(const QString& oldName, const QString& newName);
    void deleteBranchRequested(const QString& branch);

private:
    void refresh();
    void onOperationFinished(int kind, bool succeeded, const QString& message);
    void populateBranchSubmenu(QMenu* branchMenu, const QString& branchName, bool isLocal);

    QPointer<GitController> m_controller;
    QPointer<ListPopup> m_popup;
    QPointer<InputPopup> m_prompt;
    QPointer<QMenu> m_menu;
    QString m_lastError;
};

} // namespace frost::ui
