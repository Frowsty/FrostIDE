// SPDX-License-Identifier: Apache-2.0
//
// TrustDialog and TrustPrompter (frost/ui/toolwindows/trust_dialog.hpp).

#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "tool_icons.hpp"
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>

namespace frost::ui {
namespace {

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace

TrustDialog::TrustDialog(BuildController& controller, const QStringList& roots, QWidget* parent, TrustPurpose purpose)
    : QDialog(parent), m_controller(controller), m_purpose(purpose), m_list(new QListWidget(this)),
      m_error(new QLabel(this)) {
    setObjectName(QStringLiteral("trustDialog"));
    const bool attach = purpose == TrustPurpose::DebugAttach;
    setWindowTitle(attach ? tr("Attach Debugger?") : tr("Trust and Open Project?"));

    const auto& solution = controller.solution();
    const QString name =
        solution           ? QString::fromUtf8(solution->name().data(), static_cast<qsizetype>(solution->name().size()))
        : !roots.isEmpty() ? QFileInfo(roots.front()).fileName()
                           : QString();
    auto* heading = new QLabel(attach           ? tr("Let the debugger attach?")
                               : name.isEmpty() ? tr("Trust and open project?")
                                                : tr("Trust and open project '%1'?").arg(name),
                               this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    m_body = new QLabel(explanation(), this);
    m_body->setObjectName(QStringLiteral("trustDescription"));
    m_body->setWordWrap(true);

    m_list->setObjectName(QStringLiteral("trustRoots"));
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setFocusPolicy(Qt::NoFocus);

    m_error->setObjectName(QStringLiteral("trustError"));
    m_error->setWordWrap(true);
    QPalette errorPalette = m_error->palette();
    errorPalette.setColor(QPalette::WindowText, detail::iconRoleColor(IconRole::Error, QColor(0xd0, 0x30, 0x30)));
    m_error->setPalette(errorPalette);
    m_error->hide();

    auto* buttons = new QDialogButtonBox(this);
    m_trust = buttons->addButton(attach ? tr("Trust and Attach") : tr("Trust"), QDialogButtonBox::AcceptRole);
    m_trust->setObjectName(QStringLiteral("trust"));
    m_trust->setDefault(true);
    m_cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_cancel->setObjectName(QStringLiteral("cancel"));
    // Wired directly (not through the box's accepted/rejected) so trusting decides whether to close.
    connect(m_trust, &QPushButton::clicked, this, &TrustDialog::trustAll);
    connect(m_cancel, &QPushButton::clicked, this, &TrustDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(m_body);
    layout->addWidget(m_list);
    layout->addWidget(m_error);
    layout->addWidget(buttons);
    installDialogChrome(this, false);
    setRoots(roots);
}

TrustDialog::~TrustDialog() = default;

QString TrustDialog::explanation() const {
    switch (m_purpose) {
    case TrustPurpose::GitRepository:
        return tr("Git operations can run repository hooks, filters and configured helpers. Trust these folders only "
                  "if you know where they come from:");
    case TrustPurpose::LuaLanguageServer:
        return tr("The Lua language server reads project configuration and can load project-selected scripts and "
                  "extensions. "
                  "Trust it only if you know where it comes from. Trusting covers these folders and everything below "
                  "them:");
    case TrustPurpose::ManagedLanguageServer:
        return tr("The C# language server evaluates project files and may run project-selected code. Trust it only "
                  "if you know where it comes from. Trusting covers these folders and everything below them:");
    case TrustPurpose::ProjectOpen:
        return tr("Opening, configuring, or building this project or folder can run project-controlled code: "
                  "CMake scripts, MSBuild targets, build events, and language servers (including the Lua language "
                  "server and C# language server) that read project configuration and load scripts or extensions. "
                  "Trust it only if you know where it comes from. Trusting covers these folders and everything below "
                  "them:");
    case TrustPurpose::DebugAttach:
        // Attaching starts nothing from the folder; the risk is the access the debugger gets.
        return m_sessionGrant
                   ? tr("Attaching runs no project code, but it gives the debugger full access to the running "
                        "process: it can read and change its memory and stop it. No project is open, so this "
                        "permission is for this attach only and is not saved; the folder does not become a trusted "
                        "project. The program's folder:")
                   : tr("Attaching runs no project code, but it gives the debugger full access to the running "
                        "process: it can read and change its memory and stop it. Trust the project only if you know "
                        "where it comes from. Trusting covers these folders and everything below them:");
    case TrustPurpose::Build: break;
    }
    return tr("Configuring or building runs code from the project: CMake scripts, MSBuild targets, "
              "build events and custom build steps. "
              "Trust it only if you know where it comes from. Trusting covers these folders and everything below "
              "them:");
}

void TrustDialog::setSessionGrant(std::function<Result<void>(std::string_view root)> grant) {
    m_sessionGrant = std::move(grant);
    m_body->setText(explanation());
    if (m_purpose == TrustPurpose::DebugAttach)
        m_trust->setText(m_sessionGrant ? tr("Attach Once") : tr("Trust and Attach"));
}

QString TrustDialog::description() const { return m_body->text(); }

void TrustDialog::setRoots(const QStringList& roots) {
    m_roots = roots;
    m_list->clear();
    detail::IconCache icons;
    for (const QString& root : m_roots) {
        auto* item = new QListWidgetItem(icons.get(icons::kNodesFolder), root, m_list);
        item->setToolTip(root);
    }
    m_trust->setEnabled(!m_roots.isEmpty());
}

QString TrustDialog::errorText() const { return m_error->isHidden() ? QString() : m_error->text(); }

void TrustDialog::trustAll() {
    m_trusting = true;
    const QStringList roots = m_roots;
    for (qsizetype i = 0; i < roots.size(); ++i) {
        const std::string root = toUtf8(roots[i]);
        auto trusted = m_sessionGrant ? m_sessionGrant(root) : m_controller.trust(root);
        if (!trusted) {
            const std::string& message = trusted.error().message();
            m_error->setText(
                tr("Could not trust %1: %2")
                    .arg(roots[i], QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()))));
            m_error->show();
            setRoots(roots.mid(i)); // The folders still untrusted, the failed one first.
            m_trusting = false;
            return;
        }
    }
    m_trusting = false;
    accept();
}

void TrustDialog::reject() {
    // A build prompt owns the waiting build; a Lua, managed, repository, project-open or attach
    // prompt may coexist with it but does not.
    if (m_purpose == TrustPurpose::Build && !m_trusting && m_controller.state() == build::BuildState::NeedsTrust) {
        m_controller.cancel();
    }
    QDialog::reject();
}

// ---- TrustPrompter.

TrustPrompter::TrustPrompter(BuildController& controller, QWidget* window, QObject* parent)
    : QObject(parent), m_controller(controller), m_window(window) {
    connect(&m_controller, &BuildController::needsTrust, this, &TrustPrompter::prompt);
    connect(&m_controller, &BuildController::stateChanged, this, [this](build::BuildState state) {
        if (state != build::BuildState::NeedsTrust && m_dialog) {
            m_dialog->done(QDialog::Rejected); // Not reject(): the build already moved on.
        }
    });
}

TrustPrompter::~TrustPrompter() {
    if (m_dialog) {
        m_dialog->done(QDialog::Rejected);
    }
}

void TrustPrompter::prompt(const QStringList& roots) {
    if (m_dialog) {
        m_dialog->setRoots(roots);
        return;
    }
    auto* dialog = new TrustDialog(m_controller, roots, m_window.data());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    m_dialog = dialog;
    dialog->open();
}

} // namespace frost::ui
