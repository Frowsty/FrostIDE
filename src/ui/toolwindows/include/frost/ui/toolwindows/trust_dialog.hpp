// SPDX-License-Identifier: Apache-2.0
//
// The trust prompt (W1, ADR 0039; PLAN.md section 3.5): TrustDialog asks "Trust and Open
// Project?" for the folders a build waiting in NeedsTrust would run code from, and TrustPrompter
// shows it whenever the BuildController reports NeedsTrust.
//
/*
 * Trust calls BuildController::trust for each listed folder in order; once all are trusted the
 * build service continues the build on its own and the dialog closes (accepted). A folder that
 * cannot be trusted stops there: the error shows in the dialog, which stays open, and the folders
 * still untrusted can be tried again. Cancel (or closing) cancels a waiting build only for a
 * Build-purpose dialog; declining Lua language-server trust, or a debugger attach, leaves an
 * unrelated build alone.
 *
 * DebugAttach (Attach to Process) says that attaching runs no project code but gives the debugger
 * full access to the process. With setSessionGrant, Trust calls that grant instead of
 * BuildController::trust, and the dialog says the permission is not saved: the workbench uses it
 * for a one-off attach with no project open, so a system folder never becomes a trusted project.
 * TrustPrompter keeps at most one dialog: a repeated NeedsTrust updates the open one's list, and
 * the dialog closes by itself when the build leaves NeedsTrust some other way.
 *
 * GUI thread only; the controller must outlive the dialog and the prompter. The dialog is
 * window-modal over its parent when shown through TrustPrompter (open(), never exec()).
 */
#pragma once

#include "frost/core/error.hpp"

#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <string_view>

class QLabel;
class QListWidget;
class QPushButton;

namespace frost::ui {

class BuildController;

// The operation whose project-controlled content needs trust.
enum class TrustPurpose { Build, LuaLanguageServer, ManagedLanguageServer, GitRepository, ProjectOpen, DebugAttach };

class TrustDialog final : public QDialog {
    Q_OBJECT

public:
    TrustDialog(BuildController& controller, const QStringList& roots, QWidget* parent = nullptr,
                TrustPurpose purpose = TrustPurpose::Build);
    ~TrustDialog() override;

    // Replaces the listed folders (a repeated NeedsTrust).
    void setRoots(const QStringList& roots);
    /*
     * Trust calls `grant` for each listed folder instead of BuildController::trust (nothing is saved
     * through the controller); the explanation and the button say the permission is for this
     * operation only. Call before the dialog is shown. Null restores the controller's trust.
     */
    void setSessionGrant(std::function<Result<void>(std::string_view root)> grant);
    [[nodiscard]] bool sessionOnly() const noexcept { return static_cast<bool>(m_sessionGrant); }
    [[nodiscard]] TrustPurpose purpose() const noexcept { return m_purpose; }
    // The explanation shown above the folder list.
    [[nodiscard]] QString description() const;
    [[nodiscard]] const QStringList& roots() const noexcept { return m_roots; }

    [[nodiscard]] QPushButton* trustButton() const noexcept { return m_trust; }
    [[nodiscard]] QPushButton* cancelButton() const noexcept { return m_cancel; }
    // The error of the last failed trust(), or "".
    [[nodiscard]] QString errorText() const;

    void reject() override;

private:
    void trustAll();

    // The explanation for the purpose and the session-only mode.
    [[nodiscard]] QString explanation() const;

    BuildController& m_controller;
    TrustPurpose m_purpose;
    std::function<Result<void>(std::string_view root)> m_sessionGrant;
    QStringList m_roots;
    QLabel* m_body = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_trust = nullptr;
    QPushButton* m_cancel = nullptr;
    bool m_trusting = false;
};

// Opens a TrustDialog over `window` for each NeedsTrust of `controller` (header comment).
class TrustPrompter final : public QObject {
    Q_OBJECT

public:
    TrustPrompter(BuildController& controller, QWidget* window, QObject* parent = nullptr);
    ~TrustPrompter() override;

    // The open dialog, or nullptr.
    [[nodiscard]] TrustDialog* dialog() const noexcept { return m_dialog.data(); }

private:
    void prompt(const QStringList& roots);

    BuildController& m_controller;
    QPointer<QWidget> m_window;
    QPointer<TrustDialog> m_dialog;
};

} // namespace frost::ui
