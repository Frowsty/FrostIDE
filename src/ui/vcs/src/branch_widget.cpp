// SPDX-License-Identifier: Apache-2.0
//
// BranchStatusWidget and the Branches popup (frost/ui/vcs/branch_widget.hpp).

#include "frost/ui/vcs/branch_widget.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/vcs/git_controller.hpp"
#include "frost/vcs/git_service.hpp"

#include <QLineEdit>

namespace frost::ui {
namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

std::vector<ListPopupItem> branchPopupItems(const GitController& controller) {
    std::vector<ListPopupItem> items;
    items.push_back(ListPopupItem{QObject::tr("New Branch..."), {}, {}, QStringLiteral("new:")});
    for (const bool remote : {false, true}) {
        for (const vcs::GitBranch& branch : controller.branches()) {
            if (branch.remote != remote) {
                continue;
            }
            QString detail = branch.current ? QObject::tr("current") : QString();
            if (!branch.upstream.empty()) {
                detail += (detail.isEmpty() ? QString() : QStringLiteral(", ")) + qs(branch.upstream);
            }
            if (remote) {
                detail = QObject::tr("remote");
            }
            items.push_back(
                ListPopupItem{qs(branch.name),
                              detail,
                              {},
                              (remote ? QStringLiteral("remote:") : QStringLiteral("local:")) + qs(branch.name)});
        }
    }
    return items;
}

BranchStatusWidget::BranchStatusWidget(GitController& controller, QWidget* parent)
    : QToolButton(parent), m_controller(&controller) {
    setObjectName(QStringLiteral("gitBranchWidget"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        setIcon(style->icon(icons::kVcsBranch));
    }
    connect(this, &QToolButton::clicked, this, [this] { openPopup(); });
    connect(&controller, &GitController::statusChanged, this, &BranchStatusWidget::refresh);
    connect(&controller, &GitController::operationFinished, this, &BranchStatusWidget::onOperationFinished);
    refresh();
}

BranchStatusWidget::~BranchStatusWidget() {
    if (m_popup) {
        m_popup->deleteLater();
    }
    if (m_prompt) {
        m_prompt->deleteLater();
    }
}

void BranchStatusWidget::refresh() {
    if (!m_controller || !m_controller->snapshot()) {
        setText(tr("Git"));
        return;
    }
    const vcs::GitBranchStatus& branch = m_controller->snapshot()->status.branch;
    if (branch.detached()) {
        setText(branch.oid.size() > 8 ? qs(branch.oid).left(8) : qs(branch.oid));
    } else {
        setText(qs(branch.head));
    }
    QString tip = branch.detached() ? tr("Detached HEAD") : tr("Git branch: %1").arg(qs(branch.head));
    if (branch.upstream) {
        tip +=
            tr("\nUpstream: %1 (%2 ahead, %3 behind)").arg(qs(*branch.upstream)).arg(branch.ahead).arg(branch.behind);
    }
    if (!m_lastError.isEmpty()) {
        tip += QStringLiteral("\n\n") + m_lastError;
    }
    setToolTip(tip);
}

ListPopup* BranchStatusWidget::openPopup() {
    if (!m_controller) {
        return nullptr;
    }
    if (m_popup) {
        m_popup->closePopup(Popup::CloseReason::Dismissed);
    }
    m_controller->readBranches();
    auto* popup = new ListPopup(window(), tr("Git Branches"), true);
    popup->setItems(branchPopupItems(*m_controller));
    // The list arrives (again) from the service; refresh the rows while the popup is open.
    connect(m_controller, &GitController::branchesChanged, popup, [this, popup] {
        if (m_controller) {
            popup->setItems(branchPopupItems(*m_controller));
        }
    });
    connect(popup, &ListPopup::itemAccepted, this, [this](int, const QVariant& data) { runItem(data); });
    m_popup = popup;
    popup->showBelow(this);
    return popup;
}

void BranchStatusWidget::runItem(const QVariant& data) {
    if (!m_controller) {
        return;
    }
    const QString value = data.toString();
    if (value == QStringLiteral("new:")) {
        auto* prompt = new InputPopup(window(), tr("New branch name:"));
        prompt->setValidator([](const QString& name) -> std::optional<QString> {
            const QString trimmed = name.trimmed();
            if (trimmed.isEmpty()) {
                return tr("Enter a branch name");
            }
            if (trimmed.contains(QLatin1Char(' ')) || trimmed.contains(QStringLiteral("..")) ||
                trimmed.startsWith(QLatin1Char('-'))) {
                return tr("'%1' is not a valid branch name").arg(trimmed);
            }
            return std::nullopt;
        });
        connect(prompt, &InputPopup::submitted, this, [this](const QString& name) {
            if (m_controller) {
                m_controller->createBranch(name);
            }
        });
        m_prompt = prompt;
        prompt->showCentered(window());
        return;
    }
    if (value.startsWith(QStringLiteral("local:"))) {
        m_controller->checkout(value.mid(6));
    } else if (value.startsWith(QStringLiteral("remote:"))) {
        m_controller->checkout(value.mid(7));
    }
}

void BranchStatusWidget::onOperationFinished(int kind, bool succeeded, const QString& message) {
    const auto operation = static_cast<vcs::GitOperationKind>(kind);
    if (operation != vcs::GitOperationKind::Checkout && operation != vcs::GitOperationKind::CreateBranch) {
        return;
    }
    m_lastError = succeeded ? QString() : message;
    refresh();
}

} // namespace frost::ui
