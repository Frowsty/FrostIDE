// SPDX-License-Identifier: Apache-2.0
//
// BranchStatusWidget and the Branches popup (frost/ui/vcs/branch_widget.hpp).

#include "frost/ui/vcs/branch_widget.hpp"

#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/vcs/git_controller.hpp"
#include "frost/vcs/git_service.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

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
    connect(this, &QToolButton::clicked, this, [this] { openMenu(); });
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
    if (m_menu) {
        m_menu->deleteLater();
    }
}

void BranchStatusWidget::refresh() {
    if (!m_controller || !m_controller->snapshot()) {
        setText(tr("Git"));
        return;
    }
    const vcs::GitBranchStatus& branch = m_controller->snapshot()->status.branch;
    QString badge = branch.detached() ? (branch.oid.size() > 8 ? qs(branch.oid).left(8) : qs(branch.oid))
                                      : qs(branch.head);
    if (branch.ahead > 0) {
        badge += QStringLiteral(" \u2191%1").arg(branch.ahead);
    }
    if (branch.behind > 0) {
        badge += QStringLiteral(" \u2193%1").arg(branch.behind);
    }
    setText(badge);
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
    } else if (value.startsWith(QStringLiteral("checkout:"))) {
        m_controller->checkout(value.mid(9));
    } else if (value == QStringLiteral("fetch:")) {
        emit fetchRequested();
        m_controller->fetch();
    } else if (value == QStringLiteral("pull:")) {
        emit pullRequested();
        m_controller->pull();
    } else if (value == QStringLiteral("push:")) {
        emit pushRequested();
        m_controller->push();
    } else if (value == QStringLiteral("stash:")) {
        emit stashRequested();
        promptStashChanges();
    } else if (value == QStringLiteral("unstash:")) {
        emit unstashRequested();
        promptUnstashChanges();
    } else if (value.startsWith(QStringLiteral("merge:"))) {
        const QString branch = value.mid(6);
        emit mergeRequested(branch);
        m_controller->mergeBranch(branch);
    } else if (value.startsWith(QStringLiteral("rebase:"))) {
        const QString branch = value.mid(7);
        emit rebaseRequested(branch);
        m_controller->rebaseBranch(branch);
    } else if (value.startsWith(QStringLiteral("rename:"))) {
        promptRenameBranch(value.mid(7));
    } else if (value.startsWith(QStringLiteral("delete:"))) {
        const QString branch = value.mid(7);
        emit deleteBranchRequested(branch);
        m_controller->deleteBranch(branch, false);
    }
}

void BranchStatusWidget::populateBranchSubmenu(QMenu* branchMenu, const QString& branchName, bool isLocal) {
    QAction* checkoutAct = branchMenu->addAction(tr("Checkout"));
    connect(checkoutAct, &QAction::triggered, this,
            [this, branchName] { runItem(QStringLiteral("checkout:") + branchName); });

    QAction* mergeAct = branchMenu->addAction(tr("Merge '%1' into Current").arg(branchName));
    connect(mergeAct, &QAction::triggered, this,
            [this, branchName] { runItem(QStringLiteral("merge:") + branchName); });

    QAction* rebaseAct = branchMenu->addAction(tr("Rebase Current onto '%1'").arg(branchName));
    connect(rebaseAct, &QAction::triggered, this,
            [this, branchName] { runItem(QStringLiteral("rebase:") + branchName); });

    if (isLocal) {
        branchMenu->addSeparator();
        QAction* renameAct = branchMenu->addAction(tr("Rename Branch..."));
        connect(renameAct, &QAction::triggered, this,
                [this, branchName] { runItem(QStringLiteral("rename:") + branchName); });

        QAction* deleteAct = branchMenu->addAction(tr("Delete Branch"));
        connect(deleteAct, &QAction::triggered, this,
                [this, branchName] { runItem(QStringLiteral("delete:") + branchName); });
    }
}

void BranchStatusWidget::populateMenu(QMenu& menu) {
    menu.setTitle(tr("Git Branches"));

    QAction* newBranchAct = menu.addAction(tr("New Branch..."));
    connect(newBranchAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("new:")); });

    QAction* fetchAct = menu.addAction(tr("Fetch"));
    connect(fetchAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("fetch:")); });

    QAction* pullAct = menu.addAction(tr("Pull"));
    connect(pullAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("pull:")); });

    QAction* pushAct = menu.addAction(tr("Push..."));
    connect(pushAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("push:")); });

    QAction* stashAct = menu.addAction(tr("Stash Changes..."));
    connect(stashAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("stash:")); });

    QAction* unstashAct = menu.addAction(tr("Unstash Changes..."));
    connect(unstashAct, &QAction::triggered, this, [this] { runItem(QStringLiteral("unstash:")); });

    if (m_controller != nullptr) {
        menu.addSeparator();
        menu.addSection(tr("Local Branches"));
        for (const vcs::GitBranch& branch : m_controller->branches()) {
            if (branch.remote) {
                continue;
            }
            const QString name = qs(branch.name);
            const QString title = branch.current ? tr("%1 (current)").arg(name) : name;
            QMenu* sub = menu.addMenu(title);
            populateBranchSubmenu(sub, name, true);
        }
        menu.addSeparator();
        menu.addSection(tr("Remote Branches"));
        for (const vcs::GitBranch& branch : m_controller->branches()) {
            if (!branch.remote) {
                continue;
            }
            const QString name = qs(branch.name);
            QMenu* sub = menu.addMenu(name);
            populateBranchSubmenu(sub, name, false);
        }
    }
}

QMenu* BranchStatusWidget::buildMenu(QWidget* parent) {
    auto* menu = new QMenu(parent != nullptr ? parent : this);
    populateMenu(*menu);
    return menu;
}

QMenu* BranchStatusWidget::openMenu() {
    if (!m_controller) {
        return nullptr;
    }
    if (m_menu) {
        m_menu->close();
        m_menu->deleteLater();
    }
    m_controller->readBranches();
    m_menu = buildMenu(this);
    m_menu->popup(mapToGlobal(QPoint(0, height())));
    return m_menu;
}

void BranchStatusWidget::promptRenameBranch(const QString& branch) {
    if (!m_controller || branch.isEmpty()) {
        return;
    }
    QDialog dialog(window());
    dialog.setWindowTitle(tr("Rename Branch '%1'").arg(branch));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* nameEdit = new QLineEdit(branch, &dialog);
    form->addRow(tr("New name:"), nameEdit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    nameEdit->selectAll();
    nameEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        const QString newName = nameEdit->text().trimmed();
        if (!newName.isEmpty() && newName != branch) {
            emit renameBranchRequested(branch, newName);
            m_controller->renameBranch(branch, newName);
        }
    }
}

void BranchStatusWidget::promptStashChanges() {
    if (!m_controller) {
        return;
    }
    QDialog dialog(window());
    dialog.setWindowTitle(tr("Stash Changes"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* messageEdit = new QLineEdit(&dialog);
    messageEdit->setPlaceholderText(tr("Optional stash message..."));
    auto* untrackedCheck = new QCheckBox(tr("Include untracked files"), &dialog);
    untrackedCheck->setChecked(true);
    form->addRow(tr("Message:"), messageEdit);
    form->addRow(QString(), untrackedCheck);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    messageEdit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        m_controller->stashPush(messageEdit->text().trimmed(), untrackedCheck->isChecked());
    }
}

void BranchStatusWidget::promptUnstashChanges() {
    if (!m_controller) {
        return;
    }
    QPointer<BranchStatusWidget> self(this);
    m_controller->readStashes([self](Result<std::vector<vcs::GitStashEntry>> result) {
        if (!self || !self->m_controller || !result) {
            return;
        }
        QDialog dialog(self->window());
        dialog.setWindowTitle(tr("Unstash Changes"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        for (const vcs::GitStashEntry& entry : *result) {
            auto* item = new QListWidgetItem(QStringLiteral("%1: %2").arg(qs(entry.ref), qs(entry.message)), list);
            item->setData(Qt::UserRole, entry.index);
        }
        if (list->count() > 0) {
            list->setCurrentRow(0);
        }
        layout->addWidget(new QLabel(tr("Saved Stashes:"), &dialog));
        layout->addWidget(list);
        auto* btnRow = new QHBoxLayout();
        auto* popBtn = new QPushButton(tr("Pop Stash"), &dialog);
        auto* applyBtn = new QPushButton(tr("Apply Stash"), &dialog);
        auto* dropBtn = new QPushButton(tr("Drop Stash"), &dialog);
        auto* closeBtn = new QPushButton(tr("Close"), &dialog);
        const bool hasStashes = list->count() > 0;
        popBtn->setEnabled(hasStashes);
        applyBtn->setEnabled(hasStashes);
        dropBtn->setEnabled(hasStashes);
        btnRow->addWidget(popBtn);
        btnRow->addWidget(applyBtn);
        btnRow->addWidget(dropBtn);
        btnRow->addStretch(1);
        btnRow->addWidget(closeBtn);
        layout->addLayout(btnRow);
        QObject::connect(popBtn, &QPushButton::clicked, &dialog, [&] {
            if (list->currentItem() != nullptr && self && self->m_controller) {
                self->m_controller->stashApply(list->currentItem()->data(Qt::UserRole).toInt(), true);
                dialog.accept();
            }
        });
        QObject::connect(applyBtn, &QPushButton::clicked, &dialog, [&] {
            if (list->currentItem() != nullptr && self && self->m_controller) {
                self->m_controller->stashApply(list->currentItem()->data(Qt::UserRole).toInt(), false);
                dialog.accept();
            }
        });
        QObject::connect(dropBtn, &QPushButton::clicked, &dialog, [&] {
            if (list->currentItem() != nullptr && self && self->m_controller) {
                self->m_controller->stashDrop(list->currentItem()->data(Qt::UserRole).toInt());
                delete list->takeItem(list->currentRow());
            }
        });
        QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
        installDialogChrome(&dialog, true);
        dialog.exec();
    });
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
