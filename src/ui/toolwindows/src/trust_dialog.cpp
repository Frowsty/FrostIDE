// SPDX-License-Identifier: Apache-2.0
//
// TrustDialog and TrustPrompter (frost/ui/toolwindows/trust_dialog.hpp).

#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "tool_icons.hpp"
#include <QDialogButtonBox>
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
    setWindowTitle(tr("Trust and Open Project?"));

    const auto& solution = controller.solution();
    const QString name =
        solution ? QString::fromUtf8(solution->name().data(), static_cast<qsizetype>(solution->name().size()))
                 : QString();
    auto* heading =
        new QLabel(name.isEmpty() ? tr("Trust and open project?") : tr("Trust and open project '%1'?").arg(name), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    const QString explanation =
        purpose == TrustPurpose::GitRepository
            ? tr("Git operations can run repository hooks, filters and configured helpers. Trust these folders only "
                 "if you know where they come from:")
        : purpose == TrustPurpose::LuaLanguageServer
            ? tr("The Lua language server reads project configuration and can load project-selected scripts and "
                 "extensions. "
                 "Trust it only if you know where it comes from. Trusting covers these folders and everything below "
                 "them:")
        : purpose == TrustPurpose::ManagedLanguageServer
            ? tr("The C# language server evaluates project files and may run project-selected code. Trust it only "
                 "if you know where it comes from. Trusting covers these folders and everything below them:")
            : tr("Configuring or building runs code from the project: CMake scripts, MSBuild targets, "
                 "build events and custom build steps. "
                 "Trust it only if you know where it comes from. Trusting covers these folders and everything below "
                 "them:");
    auto* body = new QLabel(explanation, this);
    body->setObjectName(QStringLiteral("trustDescription"));
    body->setWordWrap(true);

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
    m_trust = buttons->addButton(tr("Trust"), QDialogButtonBox::AcceptRole);
    m_trust->setObjectName(QStringLiteral("trust"));
    m_trust->setDefault(true);
    m_cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_cancel->setObjectName(QStringLiteral("cancel"));
    // Wired directly (not through the box's accepted/rejected) so trusting decides whether to close.
    connect(m_trust, &QPushButton::clicked, this, &TrustDialog::trustAll);
    connect(m_cancel, &QPushButton::clicked, this, &TrustDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(body);
    layout->addWidget(m_list);
    layout->addWidget(m_error);
    layout->addWidget(buttons);
    setRoots(roots);
}

TrustDialog::~TrustDialog() = default;

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
        auto trusted = m_controller.trust(toUtf8(roots[i]));
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
    // A build prompt owns the waiting build; a Lua prompt may coexist with it but does not.
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
