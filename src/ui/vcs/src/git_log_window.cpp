// SPDX-License-Identifier: Apache-2.0
//
// GitLogToolWindow implementation (frost/ui/vcs/git_log_window.hpp): commit graph, details and actions.

#include "frost/ui/vcs/git_log_window.hpp"

#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/vcs/git_controller.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace frost::ui {
namespace {

constexpr int kRoleCommitHash = Qt::UserRole;
constexpr int kRoleCommitIndex = Qt::UserRole + 1;
constexpr int kRoleGraphLane = Qt::UserRole + 2;
constexpr int kRoleGraphActiveLanes = Qt::UserRole + 3;
constexpr int kRoleGraphMergeLanes = Qt::UserRole + 4;
constexpr int kRoleRefsList = Qt::UserRole + 5;

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

QColor laneColor(int lane) {
    static constexpr std::array<QRgb, 6> kPalette = {
        0x4FA3FFU, 0x57C785U, 0xF5A623U, 0xD96B9CU, 0x9B78FFU, 0x4DD0E1U,
    };
    return QColor::fromRgb(kPalette[static_cast<std::size_t>(std::max(0, lane)) % kPalette.size()]);
}

struct GraphRowLayout {
    int lane = 0;
    QVariantList activeLanes;
    QVariantList mergeLanes;
};

int assignOrFindLane(std::vector<std::string>& lanes, const std::string& hash) {
    auto existing = std::ranges::find(lanes, hash);
    if (existing != lanes.end()) {
        return static_cast<int>(std::distance(lanes.begin(), existing));
    }
    auto emptyIt = std::ranges::find(lanes, std::string{});
    if (emptyIt != lanes.end()) {
        *emptyIt = hash;
        return static_cast<int>(std::distance(lanes.begin(), emptyIt));
    }
    lanes.push_back(hash);
    return static_cast<int>(lanes.size() - 1);
}

std::vector<GraphRowLayout> computeGraphLayout(const std::vector<vcs::GitCommitEntry>& commits) {
    std::vector<GraphRowLayout> rows;
    rows.reserve(commits.size());
    std::vector<std::string> lanes;
    for (const vcs::GitCommitEntry& commit : commits) {
        const int commitLane = assignOrFindLane(lanes, commit.hash);
        GraphRowLayout row;
        row.lane = commitLane;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            if (!lanes[i].empty()) {
                row.activeLanes.push_back(static_cast<int>(i));
            }
        }
        for (std::size_t i = static_cast<std::size_t>(commitLane) + 1; i < lanes.size(); ++i) {
            if (lanes[i] == commit.hash) {
                lanes[i].clear();
            }
        }
        if (commit.parents.empty()) {
            lanes[static_cast<std::size_t>(commitLane)].clear();
        } else {
            lanes[static_cast<std::size_t>(commitLane)] = commit.parents.front();
            for (std::size_t p = 1; p < commit.parents.size(); ++p) {
                row.mergeLanes.push_back(assignOrFindLane(lanes, commit.parents[p]));
            }
        }
        while (!lanes.empty() && lanes.back().empty()) {
            lanes.pop_back();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

class GitLogItemDelegate final : public QStyledItemDelegate {
public:
    explicit GitLogItemDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.column() == 0) {
            paintGraphCell(painter, option, index);
            return;
        }
        if (index.column() == 1) {
            paintSubjectCell(painter, option, index);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

private:
    void paintGraphCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        if (opt.widget != nullptr) {
            opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const int commitLane = index.data(kRoleGraphLane).toInt();
        const QVariantList active = index.data(kRoleGraphActiveLanes).toList();
        const QVariantList merges = index.data(kRoleGraphMergeLanes).toList();
        constexpr int kStep = 14;
        constexpr int kLeftPad = 10;
        const int midY = opt.rect.center().y();
        for (const QVariant& v : active) {
            const int lane = v.toInt();
            const int x = opt.rect.left() + kLeftPad + lane * kStep;
            painter->setPen(QPen(laneColor(lane), 2.0));
            painter->drawLine(x, opt.rect.top(), x, opt.rect.bottom());
        }
        const int commitX = opt.rect.left() + kLeftPad + commitLane * kStep;
        for (const QVariant& v : merges) {
            const int targetLane = v.toInt();
            const int targetX = opt.rect.left() + kLeftPad + targetLane * kStep;
            painter->setPen(QPen(laneColor(targetLane), 1.8));
            painter->drawLine(commitX, midY, targetX, opt.rect.bottom());
        }
        painter->setPen(QPen(opt.palette.base().color(), 1.5));
        painter->setBrush(laneColor(commitLane));
        painter->drawEllipse(QPoint(commitX, midY), 4, 4);
        painter->restore();
    }

    void paintSubjectCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
        const QStringList refs = index.data(kRoleRefsList).toStringList();
        if (refs.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString subject = opt.text;
        opt.text.clear();
        if (opt.widget != nullptr) {
            opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        int x = opt.rect.left() + 6;
        const int badgeHeight = std::max(14, opt.rect.height() - 6);
        const int badgeTop = opt.rect.top() + (opt.rect.height() - badgeHeight) / 2;
        QFont badgeFont = opt.font;
        badgeFont.setPointSizeF(std::max(7.5, opt.font.pointSizeF() - 1.0));
        badgeFont.setBold(true);
        painter->setFont(badgeFont);
        const QFontMetrics badgeFm(badgeFont);
        for (const QString& ref : refs) {
            const int textWidth = badgeFm.horizontalAdvance(ref);
            const QRect pill(x, badgeTop, textWidth + 12, badgeHeight);
            QColor bg = QColor::fromRgb(0x2E5C46U);
            if (ref.startsWith(QStringLiteral("tag:"))) {
                bg = QColor::fromRgb(0x6E521EU);
            } else if (ref.contains(QLatin1Char('/'))) {
                bg = QColor::fromRgb(0x284B78U);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(bg);
            painter->drawRoundedRect(pill, 4.0, 4.0);
            painter->setPen(QColor::fromRgb(0xF0F4F8U));
            painter->drawText(pill, Qt::AlignCenter, ref);
            x += pill.width() + 5;
        }
        painter->setFont(opt.font);
        painter->setPen(opt.state & QStyle::State_Selected ? opt.palette.highlightedText().color()
                                                           : opt.palette.text().color());
        const QRect textRect(x, opt.rect.top(), std::max(0, opt.rect.right() - x - 4), opt.rect.height());
        const QString elided = opt.fontMetrics.elidedText(subject, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
        painter->restore();
    }
};

QString promptTextDialog(QWidget* parent, const QString& title, const QString& labelText, const QString& initial = {}) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* edit = new QLineEdit(initial, &dialog);
    form->addRow(labelText, edit);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return edit->text().trimmed();
}

} // namespace

GitLogToolWindow::GitLogToolWindow(QWidget* parent) : QWidget(parent) { buildUi(); }

GitLogToolWindow::GitLogToolWindow(GitController* controller, QWidget* parent) : QWidget(parent) {
    buildUi();
    setController(controller);
}

void GitLogToolWindow::buildUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto* filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(6);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("gitLogSearchEdit"));
    m_searchEdit->setPlaceholderText(tr("Text or hash..."));
    m_searchEdit->setClearButtonEnabled(true);

    m_branchCombo = new QComboBox(this);
    m_branchCombo->setObjectName(QStringLiteral("gitLogBranchCombo"));
    m_branchCombo->addItem(tr("All Branches"), QString());

    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName(QStringLiteral("gitLogRefreshButton"));

    filterRow->addWidget(m_searchEdit, 1);
    filterRow->addWidget(m_branchCombo);
    filterRow->addWidget(m_refreshButton);
    rootLayout->addLayout(filterRow);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_commitTree = new QTreeWidget(m_splitter);
    m_commitTree->setObjectName(QStringLiteral("gitLogCommitTree"));
    m_commitTree->setRootIsDecorated(false);
    m_commitTree->setUniformRowHeights(true);
    m_commitTree->setAlternatingRowColors(true);
    m_commitTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commitTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commitTree->setHeaderLabels({tr("Graph"), tr("Subject"), tr("Author"), tr("Date"), tr("Hash")});
    m_commitTree->setItemDelegate(new GitLogItemDelegate(m_commitTree));
    m_commitTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_commitTree->header()->setStretchLastSection(false);
    m_commitTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_commitTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_commitTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_commitTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_commitTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_commitTree->setColumnWidth(0, 72);

    auto* rightPane = new QWidget(m_splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    m_detailsEdit = new QTextEdit(rightPane);
    m_detailsEdit->setObjectName(QStringLiteral("gitLogDetailsEdit"));
    m_detailsEdit->setReadOnly(true);
    m_filesList = new QListWidget(rightPane);
    m_filesList->setObjectName(QStringLiteral("gitLogFilesList"));
    rightLayout->addWidget(m_detailsEdit, 1);
    rightLayout->addWidget(new QLabel(tr("Changed Files"), rightPane));
    rightLayout->addWidget(m_filesList, 1);

    m_splitter->addWidget(m_commitTree);
    m_splitter->addWidget(rightPane);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    rootLayout->addWidget(m_splitter, 1);

    connect(m_searchEdit, &QLineEdit::returnPressed, this, &GitLogToolWindow::refresh);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &GitLogToolWindow::populateCommitTable);
    connect(m_branchCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updatingBranches) {
            refresh();
        }
    });
    connect(m_refreshButton, &QPushButton::clicked, this, &GitLogToolWindow::refresh);
    connect(m_commitTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { onCurrentCommitChanged(current); });
    connect(m_commitTree, &QWidget::customContextMenuRequested, this, &GitLogToolWindow::showCommitContextMenu);
    connect(m_filesList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item == nullptr || m_commitTree->currentItem() == nullptr) {
            return;
        }
        const QString hash = m_commitTree->currentItem()->data(0, kRoleCommitHash).toString();
        const QString path = item->data(Qt::UserRole).toString();
        if (!hash.isEmpty() && !path.isEmpty()) {
            emit openCommitFileRequested(hash, path);
        }
    });
}

void GitLogToolWindow::setController(GitController* controller) {
    if (m_controller == controller) {
        return;
    }
    if (m_controller != nullptr) {
        disconnect(m_controller, nullptr, this, nullptr);
    }
    m_controller = controller;
    if (m_controller != nullptr) {
        connect(m_controller, &GitController::statusChanged, this, &GitLogToolWindow::refresh);
        connect(m_controller, &GitController::branchesChanged, this, &GitLogToolWindow::rebuildBranchCombo);
        connect(m_controller, &GitController::operationFinished, this, [this] { refresh(); });
        m_controller->readBranches();
        rebuildBranchCombo();
        refresh();
    } else {
        m_commits.clear();
        m_commitTree->clear();
        m_detailsEdit->clear();
        m_filesList->clear();
    }
}

void GitLogToolWindow::rebuildBranchCombo() {
    m_updatingBranches = true;
    const QString previous = m_branchCombo->currentData().toString();
    m_branchCombo->clear();
    m_branchCombo->addItem(tr("All Branches"), QString());
    if (m_controller != nullptr) {
        for (const vcs::GitBranch& branch : m_controller->branches()) {
            const QString name = qs(branch.name);
            m_branchCombo->addItem(name, name);
        }
    }
    const int idx = m_branchCombo->findData(previous);
    if (idx >= 0) {
        m_branchCombo->setCurrentIndex(idx);
    }
    m_updatingBranches = false;
}

void GitLogToolWindow::refresh() {
    if (m_controller == nullptr) {
        return;
    }
    const QString branchFilter = m_branchCombo->currentData().toString();
    const QString searchFilter = m_searchEdit->text().trimmed();
    QPointer<GitLogToolWindow> self(this);
    m_controller->readLog(300, branchFilter, {}, [self](Result<std::vector<vcs::GitCommitEntry>> result) {
        if (!self || !result) {
            return;
        }
        self->setCommits(std::move(*result));
    });
}

void GitLogToolWindow::setCommits(std::vector<vcs::GitCommitEntry> commits) {
    m_commits = std::move(commits);
    populateCommitTable();
}

void GitLogToolWindow::populateCommitTable() {
    m_commitTree->clear();
    m_detailsEdit->clear();
    m_filesList->clear();

    const QString query = m_searchEdit->text().trimmed();
    std::vector<vcs::GitCommitEntry> filtered;
    filtered.reserve(m_commits.size());
    for (const vcs::GitCommitEntry& entry : m_commits) {
        if (!query.isEmpty()) {
            const bool match = qs(entry.subject).contains(query, Qt::CaseInsensitive) ||
                               qs(entry.hash).contains(query, Qt::CaseInsensitive) ||
                               qs(entry.shortHash).contains(query, Qt::CaseInsensitive) ||
                               qs(entry.author).contains(query, Qt::CaseInsensitive);
            if (!match) {
                continue;
            }
        }
        filtered.push_back(entry);
    }

    const std::vector<GraphRowLayout> graphRows = computeGraphLayout(filtered);
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        const vcs::GitCommitEntry& commit = filtered[i];
        const GraphRowLayout& row = graphRows[i];
        auto* item = new QTreeWidgetItem(m_commitTree);
        const QString shortDate = qs(commit.dateIso).left(19).replace(QLatin1Char('T'), QLatin1Char(' '));
        item->setText(1, qs(commit.subject));
        item->setText(2, qs(commit.author));
        item->setText(3, shortDate);
        item->setText(4, qs(commit.shortHash));
        item->setData(0, kRoleCommitHash, qs(commit.hash));
        item->setData(0, kRoleCommitIndex, static_cast<int>(i));
        item->setData(0, kRoleGraphLane, row.lane);
        item->setData(0, kRoleGraphActiveLanes, row.activeLanes);
        item->setData(0, kRoleGraphMergeLanes, row.mergeLanes);
        QStringList refs;
        for (const std::string& r : commit.refs) {
            refs.push_back(qs(r));
        }
        item->setData(1, kRoleRefsList, refs);
    }
    if (m_commitTree->topLevelItemCount() > 0) {
        m_commitTree->setCurrentItem(m_commitTree->topLevelItem(0));
    }
}

void GitLogToolWindow::onCurrentCommitChanged(QTreeWidgetItem* current) {
    m_detailsEdit->clear();
    m_filesList->clear();
    if (current == nullptr) {
        return;
    }
    const QString hash = current->data(0, kRoleCommitHash).toString();
    auto it = std::ranges::find_if(m_commits, [&](const vcs::GitCommitEntry& c) { return qs(c.hash) == hash; });
    if (it != m_commits.end()) {
        QStringList parentShort;
        for (const std::string& p : it->parents) {
            parentShort.push_back(qs(p).left(8));
        }
        QStringList refStrings;
        for (const std::string& r : it->refs) {
            refStrings.push_back(qs(r));
        }
        QString summary = tr("<b>Commit:</b> %1<br><b>Author:</b> %2 &lt;%3&gt;<br><b>Date:</b> %4")
                              .arg(qs(it->hash).toHtmlEscaped(), qs(it->author).toHtmlEscaped(),
                                   qs(it->email).toHtmlEscaped(), qs(it->dateIso).toHtmlEscaped());
        if (!parentShort.isEmpty()) {
            summary += tr("<br><b>Parents:</b> %1").arg(parentShort.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        if (!refStrings.isEmpty()) {
            summary += tr("<br><b>Refs:</b> %1").arg(refStrings.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        summary += QStringLiteral("<hr><p>") + qs(it->subject).toHtmlEscaped() + QStringLiteral("</p>");
        m_detailsEdit->setHtml(summary);
    }
    emit commitSelected(hash);
    if (m_controller == nullptr || hash.isEmpty()) {
        return;
    }
    QPointer<GitLogToolWindow> self(this);
    m_controller->readCommitFiles(hash, [self, hash](Result<std::vector<vcs::GitCommitFileChange>> files) {
        if (!self || !files || self->m_commitTree->currentItem() == nullptr) {
            return;
        }
        if (self->m_commitTree->currentItem()->data(0, kRoleCommitHash).toString() != hash) {
            return;
        }
        self->m_filesList->clear();
        for (const vcs::GitCommitFileChange& change : *files) {
            const QString label = QStringLiteral("[%1] %2").arg(qs(change.status), qs(change.path));
            auto* listItem = new QListWidgetItem(label, self->m_filesList);
            listItem->setData(Qt::UserRole, qs(change.path));
        }
    });
}

void GitLogToolWindow::showCommitContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_commitTree->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    m_commitTree->setCurrentItem(item);
    const QString hash = item->data(0, kRoleCommitHash).toString();
    if (hash.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction* cherryPickAction = menu.addAction(tr("Cherry-Pick Commit"));
    QAction* revertAction = menu.addAction(tr("Revert Commit"));
    QMenu* resetMenu = menu.addMenu(tr("Reset Current Branch to Here..."));
    QAction* resetSoftAction = resetMenu->addAction(tr("Soft"));
    QAction* resetMixedAction = resetMenu->addAction(tr("Mixed"));
    QAction* resetHardAction = resetMenu->addAction(tr("Hard"));
    menu.addSeparator();
    QAction* newBranchAction = menu.addAction(tr("New Branch from Here..."));
    QAction* newTagAction = menu.addAction(tr("New Tag..."));
    menu.addSeparator();
    QAction* copyHashAction = menu.addAction(tr("Copy Commit Hash"));

    QAction* chosen = menu.exec(m_commitTree->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) {
        return;
    }
    if (chosen == copyHashAction) {
        if (QClipboard* clipboard = QApplication::clipboard()) {
            clipboard->setText(hash);
        }
        return;
    }
    if (m_controller == nullptr) {
        return;
    }
    if (chosen == cherryPickAction) {
        m_controller->cherryPick(hash);
    } else if (chosen == revertAction) {
        m_controller->revertCommit(hash);
    } else if (chosen == resetSoftAction) {
        confirmAndResetBranch(hash, QStringLiteral("soft"));
    } else if (chosen == resetMixedAction) {
        confirmAndResetBranch(hash, QStringLiteral("mixed"));
    } else if (chosen == resetHardAction) {
        confirmAndResetBranch(hash, QStringLiteral("hard"));
    } else if (chosen == newBranchAction) {
        promptNewBranchFrom(hash);
    } else if (chosen == newTagAction) {
        promptNewTagAt(hash);
    }
}

void GitLogToolWindow::promptNewBranchFrom(const QString& commitHash) {
    if (m_controller == nullptr) {
        return;
    }
    const QString name = promptTextDialog(this, tr("New Branch"), tr("Branch name:"));
    if (!name.isEmpty()) {
        m_controller->createBranchAt(name, commitHash);
    }
}

void GitLogToolWindow::promptNewTagAt(const QString& commitHash) {
    if (m_controller == nullptr) {
        return;
    }
    const QString tag = promptTextDialog(this, tr("New Tag"), tr("Tag name:"));
    if (!tag.isEmpty()) {
        m_controller->createTag(tag, commitHash);
    }
}

void GitLogToolWindow::confirmAndResetBranch(const QString& commitHash, const QString& mode) {
    if (m_controller == nullptr) {
        return;
    }
    if (mode == QStringLiteral("hard")) {
        const auto answer = QMessageBox::warning(
            this, tr("Hard Reset"),
            tr("Resetting hard to %1 will discard all uncommitted changes in the working tree. Continue?")
                .arg(commitHash.left(8)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    m_controller->resetBranch(commitHash, mode);
}

} // namespace frost::ui
