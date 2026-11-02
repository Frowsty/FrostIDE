// SPDX-License-Identifier: Apache-2.0
//
// The Commit tool window (frost/ui/vcs/commit_window.hpp): the changes model and the window.

#include "frost/ui/vcs/commit_window.hpp"

#include "frost/ui/vcs/git_controller.hpp"
#include "frost/ui/vcs/vcs_colors.hpp"
#include "frost/vcs/vcs_status.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace frost::ui {
namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kChangesGroup = 0;
constexpr int kUnversionedGroup = 1;

// The file name and the directory of a '/' separated relative path.
std::pair<std::string_view, std::string_view> splitRelative(std::string_view relative) {
    const std::size_t slash = relative.rfind('/');
    if (slash == std::string_view::npos) {
        return {relative, {}};
    }
    return {relative.substr(slash + 1), relative.substr(0, slash)};
}

} // namespace

// ---- CommitChangesModel.

CommitChangesModel::CommitChangesModel(QObject* parent) : QStandardItemModel(parent) {
    setColumnCount(2);
    connect(this, &QStandardItemModel::itemChanged, this, &CommitChangesModel::onItemChanged);
}

void CommitChangesModel::setStatus(const std::string& workTree, const vcs::GitStatus& status) {
    m_updating = true;
    removeRows(0, rowCount());
    m_changes = nullptr;
    m_unversioned = nullptr;

    std::vector<const vcs::GitStatusEntry*> changes;
    std::vector<const vcs::GitStatusEntry*> unversioned;
    for (const vcs::GitStatusEntry& entry : status.entries) {
        if (entry.kind == vcs::GitEntryKind::Untracked) {
            unversioned.push_back(&entry);
        } else if (entry.kind != vcs::GitEntryKind::Ignored) {
            changes.push_back(&entry);
        }
    }
    const auto byPath = [](const vcs::GitStatusEntry* a, const vcs::GitStatusEntry* b) { return a->path < b->path; };
    std::ranges::sort(changes, byPath);
    std::ranges::sort(unversioned, byPath);

    std::set<std::string> listed;
    const auto addGroup = [&](const QString& title, int group, const std::vector<const vcs::GitStatusEntry*>& entries,
                              bool checkedByDefault) -> QStandardItem* {
        if (entries.empty()) {
            return nullptr;
        }
        auto* header = new QStandardItem(title);
        header->setData(group, GroupRole);
        header->setCheckable(true);
        header->setEditable(false);
        auto* count = new QStandardItem(entries.size() == 1 ? tr("1 file") : tr("%1 files").arg(entries.size()));
        count->setEditable(false);
        for (const vcs::GitStatusEntry* entry : entries) {
            const std::string path = vcs::absoluteGitPath(workTree, entry->path);
            listed.insert(path);
            const auto [name, directory] = splitRelative(entry->path);
            auto* item = new QStandardItem(qs(name));
            item->setEditable(false);
            item->setCheckable(true);
            item->setData(qs(path), PathRole);
            const vcs::VcsFileStatus fileStatus = entry->fileStatus();
            item->setData(static_cast<int>(fileStatus), StatusRole);
            QString tip = qs(entry->path);
            if (!entry->originalPath.empty()) {
                tip += tr("\nrenamed from %1").arg(qs(entry->originalPath));
            }
            tip += QStringLiteral("\n") + qs(vcs::toString(fileStatus));
            item->setToolTip(tip);
            if (const std::optional<QColor> color = vcsStatusColor(fileStatus)) {
                item->setForeground(*color);
            }
            const bool checked = m_checked.contains(path) || (!m_unchecked.contains(path) && checkedByDefault);
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            auto* where = new QStandardItem(qs(directory));
            where->setEditable(false);
            where->setForeground(QColor(0x8A, 0x90, 0x9A));
            header->appendRow({item, where});
        }
        appendRow({header, count});
        updateGroupCheck(header);
        return header;
    };
    m_changes = addGroup(tr("Changes"), kChangesGroup, changes, true);
    m_unversioned = addGroup(tr("Unversioned Files"), kUnversionedGroup, unversioned, false);
    // Forget the choices of paths that are gone.
    std::erase_if(m_checked, [&](const std::string& path) { return !listed.contains(path); });
    std::erase_if(m_unchecked, [&](const std::string& path) { return !listed.contains(path); });
    m_updating = false;
}

std::vector<std::string> CommitChangesModel::checkedPaths() const {
    std::vector<std::string> paths;
    for (QStandardItem* group : {m_changes, m_unversioned}) {
        if (group == nullptr) {
            continue;
        }
        for (int row = 0; row < group->rowCount(); ++row) {
            const QStandardItem* item = group->child(row, 0);
            if (item->checkState() == Qt::Checked) {
                paths.push_back(item->data(PathRole).toString().toStdString());
            }
        }
    }
    return paths;
}

std::vector<std::string> CommitChangesModel::allPaths() const {
    std::vector<std::string> paths;
    for (QStandardItem* group : {m_changes, m_unversioned}) {
        if (group == nullptr) {
            continue;
        }
        for (int row = 0; row < group->rowCount(); ++row) {
            paths.push_back(group->child(row, 0)->data(PathRole).toString().toStdString());
        }
    }
    return paths;
}

QStandardItem* CommitChangesModel::itemFor(const std::string& absolutePath) const {
    const QString wanted = qs(absolutePath);
    for (QStandardItem* group : {m_changes, m_unversioned}) {
        if (group == nullptr) {
            continue;
        }
        for (int row = 0; row < group->rowCount(); ++row) {
            QStandardItem* item = group->child(row, 0);
            if (item->data(PathRole).toString() == wanted) {
                return item;
            }
        }
    }
    return nullptr;
}

void CommitChangesModel::setChecked(const std::string& absolutePath, bool checked) {
    if (QStandardItem* item = itemFor(absolutePath)) {
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked); // onItemChanged records it.
    }
}

bool CommitChangesModel::isChecked(const std::string& absolutePath) const {
    const QStandardItem* item = itemFor(absolutePath);
    return item != nullptr && item->checkState() == Qt::Checked;
}

void CommitChangesModel::onItemChanged(QStandardItem* item) {
    if (m_updating || item == nullptr || item->column() != 0) {
        return;
    }
    m_updating = true;
    if (item->parent() == nullptr) {
        // A group's box: check or clear every file in it.
        const Qt::CheckState state = item->checkState() == Qt::Unchecked ? Qt::Unchecked : Qt::Checked;
        item->setCheckState(state);
        for (int row = 0; row < item->rowCount(); ++row) {
            QStandardItem* child = item->child(row, 0);
            child->setCheckState(state);
            const std::string path = child->data(PathRole).toString().toStdString();
            (state == Qt::Checked ? m_checked : m_unchecked).insert(path);
            (state == Qt::Checked ? m_unchecked : m_checked).erase(path);
        }
    } else {
        const std::string path = item->data(PathRole).toString().toStdString();
        const bool checked = item->checkState() == Qt::Checked;
        (checked ? m_checked : m_unchecked).insert(path);
        (checked ? m_unchecked : m_checked).erase(path);
        updateGroupCheck(item->parent());
    }
    m_updating = false;
}

void CommitChangesModel::updateGroupCheck(QStandardItem* group) {
    int checked = 0;
    for (int row = 0; row < group->rowCount(); ++row) {
        checked += group->child(row, 0)->checkState() == Qt::Checked ? 1 : 0;
    }
    const bool wasUpdating = m_updating;
    m_updating = true;
    group->setCheckState(checked == 0                   ? Qt::Unchecked
                         : checked == group->rowCount() ? Qt::Checked
                                                        : Qt::PartiallyChecked);
    m_updating = wasUpdating;
}

// ---- CommitToolWindow.

CommitToolWindow::CommitToolWindow(GitController& controller, QWidget* parent)
    : QWidget(parent), m_controller(&controller), m_model(new CommitChangesModel(this)), m_tree(new QTreeView(this)),
      m_message(new QPlainTextEdit(this)), m_historyButton(new QToolButton(this)),
      m_amend(new QCheckBox(tr("Amend"), this)), m_commit(new QPushButton(tr("Commit"), this)),
      m_commitAndPush(new QPushButton(tr("Commit and Push..."), this)),
      m_rollback(new QPushButton(tr("Rollback"), this)), m_result(new QLabel(this)) {
    setObjectName(QStringLiteral("commitToolWindow"));
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_message->setPlaceholderText(tr("Commit Message"));
    m_message->setTabChangesFocus(true);
    m_message->setMaximumHeight(140);
    m_historyButton->setText(tr("History"));
    m_historyButton->setToolTip(tr("Recent commit messages"));
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_commit->setDefault(true);

    auto* messageRow = new QHBoxLayout;
    messageRow->addWidget(m_amend);
    messageRow->addStretch(1);
    messageRow->addWidget(m_historyButton);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_commit);
    buttons->addWidget(m_commitAndPush);
    buttons->addStretch(1);
    buttons->addWidget(m_rollback);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_tree, 1);
    layout->addLayout(messageRow);
    layout->addWidget(m_message);
    layout->addLayout(buttons);
    layout->addWidget(m_result);

    connect(m_commit, &QPushButton::clicked, this, [this] { commit(); });
    connect(m_commitAndPush, &QPushButton::clicked, this, [this] { commitAndPush(); });
    connect(m_rollback, &QPushButton::clicked, this, [this] { rollbackSelected(); });
    connect(m_amend, &QCheckBox::toggled, this, &CommitToolWindow::onAmendToggled);
    connect(m_historyButton, &QToolButton::clicked, this, &CommitToolWindow::showHistoryMenu);
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const QString path = index.siblingAtColumn(0).data(CommitChangesModel::PathRole).toString();
        if (!path.isEmpty()) {
            emit diffRequested(path);
        }
    });
    connect(m_model, &QStandardItemModel::itemChanged, this, &CommitToolWindow::updateButtons);
    connect(m_message, &QPlainTextEdit::textChanged, this, &CommitToolWindow::updateButtons);
    connect(&controller, &GitController::statusChanged, this, &CommitToolWindow::refreshFromController);
    connect(&controller, &GitController::operationFinished, this, &CommitToolWindow::onOperationFinished);
    refreshFromController();
}

CommitToolWindow::~CommitToolWindow() = default;

QString CommitToolWindow::message() const { return m_message->toPlainText(); }

void CommitToolWindow::setMessage(const QString& message) { m_message->setPlainText(message); }

QStringList CommitToolWindow::messageHistory() const {
    return m_controller ? m_controller->messageHistory() : QStringList();
}

void CommitToolWindow::useHistoryMessage(int index) {
    const QStringList history = messageHistory();
    if (index >= 0 && index < history.size()) {
        setMessage(history[index]);
    }
}

bool CommitToolWindow::commit() { return startCommit(false); }

bool CommitToolWindow::commitAndPush() { return startCommit(true); }

bool CommitToolWindow::startCommit(bool push) {
    if (!m_controller || m_committing) {
        return false;
    }
    const std::vector<std::string> paths = m_model->checkedPaths();
    const bool amend = m_amend->isChecked();
    if (message().trimmed().isEmpty()) {
        m_result->setText(tr("Enter a commit message."));
        return false;
    }
    if (paths.empty() && !amend) {
        m_result->setText(tr("Select the files to commit."));
        return false;
    }
    m_committing = true;
    m_result->setText(push ? tr("Committing and pushing...") : tr("Committing..."));
    updateButtons();
    m_controller->commit(paths, message(), amend, push);
    return true;
}

bool CommitToolWindow::rollbackSelected() {
    if (!m_controller || m_committing) {
        return false;
    }
    const std::vector<std::string> paths = m_model->checkedPaths();
    if (paths.empty()) {
        m_result->setText(tr("Select the files to roll back."));
        return false;
    }
    const QString question =
        paths.size() == 1
            ? tr("Roll back the changes to %1? They will be lost.").arg(QString::fromStdString(paths.front()))
            : tr("Roll back the changes to %1 files? They will be lost.").arg(paths.size());
    const bool go = m_confirm ? m_confirm(tr("Rollback Changes"), question)
                              : QMessageBox::question(this, tr("Rollback Changes"), question) == QMessageBox::Yes;
    if (!go) {
        return false;
    }
    m_result->setText(tr("Rolling back..."));
    m_controller->rollback(paths);
    return true;
}

std::string CommitToolWindow::currentPath() const {
    return m_tree->currentIndex().siblingAtColumn(0).data(CommitChangesModel::PathRole).toString().toStdString();
}

bool CommitToolWindow::showDiff() {
    const std::string path = currentPath();
    if (path.empty()) {
        return false;
    }
    emit diffRequested(QString::fromStdString(path));
    return true;
}

void CommitToolWindow::keyPressEvent(QKeyEvent* event) {
    // Show Diff (IntelliJ's Ctrl+D in the Commit window); the registry's vcs.showDiff does the same.
    if (event->key() == Qt::Key_D && event->modifiers() == Qt::ControlModifier) {
        showDiff();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CommitToolWindow::refreshFromController() {
    if (!m_controller || !m_controller->snapshot()) {
        updateButtons();
        return;
    }
    const std::string current = currentPath();
    m_model->setStatus(m_controller->info().workTree, m_controller->snapshot()->status);
    m_tree->expandAll();
    m_tree->resizeColumnToContents(0);
    if (QStandardItem* item = current.empty() ? nullptr : m_model->itemFor(current)) {
        m_tree->setCurrentIndex(item->index());
    }
    updateButtons();
}

void CommitToolWindow::updateButtons() {
    const bool idle = m_controller && !m_committing;
    const bool hasFiles = !m_model->checkedPaths().empty();
    m_commit->setEnabled(idle);
    m_commitAndPush->setEnabled(idle);
    m_rollback->setEnabled(idle && hasFiles);
}

void CommitToolWindow::onAmendToggled(bool on) {
    if (!m_controller) {
        return;
    }
    if (!on) {
        // Unchecking restores what was typed, unless the user changed the loaded message.
        if (message() == m_amendMessage) {
            setMessage(m_typedBeforeAmend);
        }
        m_amendMessage.clear();
        return;
    }
    m_typedBeforeAmend = message();
    if (!m_typedBeforeAmend.trimmed().isEmpty()) {
        return;
    }
    const QPointer<CommitToolWindow> self(this);
    m_controller->readLastCommitMessage([self](Result<std::string> last) {
        if (!self || !last || !self->m_amend->isChecked() || !self->message().trimmed().isEmpty()) {
            return;
        }
        self->m_amendMessage = QString::fromStdString(*last);
        self->setMessage(self->m_amendMessage);
    });
}

void CommitToolWindow::onOperationFinished(int kind, bool succeeded, const QString& message) {
    const auto operation = static_cast<vcs::GitOperationKind>(kind);
    if (operation == vcs::GitOperationKind::Commit && m_committing) {
        m_committing = false;
        if (succeeded) {
            m_amendMessage.clear();
            m_typedBeforeAmend.clear();
            const bool blocked = m_amend->blockSignals(true);
            m_amend->setChecked(false);
            m_amend->blockSignals(blocked);
            m_message->clear();
            const auto& result = m_controller ? m_controller->lastResult() : std::nullopt;
            m_result->setText(result && !result->commit.empty()
                                  ? tr("Committed %1.").arg(QString::fromStdString(result->commit.substr(0, 8)))
                                  : tr("Committed."));
        } else {
            // The selection and the message stay; the hook's output is in the message.
            m_result->setText(tr("Commit failed: %1").arg(message));
        }
    } else if (operation == vcs::GitOperationKind::Push || operation == vcs::GitOperationKind::Rollback) {
        if (!succeeded) {
            m_result->setText(tr("%1 failed: %2").arg(QString::fromUtf8(vcs::toString(operation).data()), message));
        } else if (operation == vcs::GitOperationKind::Push) {
            m_result->setText(tr("Pushed."));
        } else {
            m_result->setText(tr("Rolled back."));
        }
    }
    updateButtons();
}

void CommitToolWindow::showHistoryMenu() {
    QMenu menu(this);
    const QStringList history = messageHistory();
    if (history.isEmpty()) {
        menu.addAction(tr("No recent messages"))->setEnabled(false);
    }
    for (int i = 0; i < history.size(); ++i) {
        const QString first = history[i].section(QLatin1Char('\n'), 0, 0);
        menu.addAction(fontMetrics().elidedText(first, Qt::ElideRight, 400), this, [this, i] { useHistoryMessage(i); });
    }
    menu.exec(m_historyButton->mapToGlobal(QPoint(0, m_historyButton->height())));
}

} // namespace frost::ui
