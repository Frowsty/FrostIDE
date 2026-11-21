// SPDX-License-Identifier: Apache-2.0
// Genuine GitHub/GitLab review pages, account identity, metadata/diffs, checkout and explicitly confirmed reviews.
#include "hosting_panel.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace frost::app {
HostingPanel::HostingPanel(HostingEnvironment environment, QWidget* parent)
    : QWidget(parent), m_environment(std::move(environment)), m_provider(new QComboBox(this)),
      m_executable(new QLineEdit(this)), m_gitExecutable(new QLineEdit(this)),
      m_host(new QLineEdit(QStringLiteral("github.com"), this)), m_repository(new QLineEdit(this)),
      m_page(new QSpinBox(this)), m_reviews(new QTreeWidget(this)), m_details(new QPlainTextEdit(this)),
      m_body(new QPlainTextEdit(this)), m_status(new QLabel(this)),
      m_cancel(new QPushButton(QStringLiteral("Cancel Request"), this)) {
    setObjectName(QStringLiteral("hostingPanel"));
    m_executable->setObjectName(QStringLiteral("hostingExecutable"));
    m_host->setObjectName(QStringLiteral("hostingHost"));
    m_repository->setObjectName(QStringLiteral("hostingRepository"));
    m_reviews->setObjectName(QStringLiteral("hostingReviews"));
    m_body->setObjectName(QStringLiteral("hostingReviewBody"));
    m_status->setObjectName(QStringLiteral("hostingStatus"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(
        new QLabel(QStringLiteral("Use your separately installed and authenticated GitHub or GitLab CLI. FrostIDE "
                                  "never reads provider credentials. Requests run only when invoked below."),
                   this));
    auto* form = new QFormLayout;
    m_provider->addItems({QStringLiteral("GitHub (gh)"), QStringLiteral("GitLab (glab)")});
    form->addRow(QStringLiteral("Provider"), m_provider);
    auto* path = new QWidget(this);
    auto* pathLayout = new QHBoxLayout(path);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton(QStringLiteral("Choose CLI..."), path);
    pathLayout->addWidget(m_executable);
    pathLayout->addWidget(browse);
    form->addRow(QStringLiteral("CLI executable"), path);
    m_gitExecutable->setObjectName(QStringLiteral("hostingGitExecutable"));
    form->addRow(QStringLiteral("Git executable (checkout verification)"), m_gitExecutable);
    form->addRow(QStringLiteral("Host"), m_host);
    form->addRow(QStringLiteral("Owner / repository"), m_repository);
    m_page->setRange(1, 10000);
    form->addRow(QStringLiteral("Page (reviews / files / threads, 100 each)"), m_page);
    layout->addLayout(form);
    m_reviews->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Author"),
                                QStringLiteral("Source → Target"), QStringLiteral("State")});
    m_reviews->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_reviews, 1);
    m_details->setReadOnly(true);
    m_details->setObjectName(QStringLiteral("hostingDetails"));
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_details, QStringLiteral("Metadata / raw diff"));
    m_lines = new QTreeWidget(this);
    m_lines->setObjectName(QStringLiteral("hostingDiffLines"));
    m_lines->setHeaderLabels({QStringLiteral("Provider path"), QStringLiteral("Old"), QStringLiteral("New"),
                              QStringLiteral("Actual patch line")});
    m_lines->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tabs->addTab(m_lines, QStringLiteral("Review lines"));
    auto* discussions = new QWidget(this);
    auto* threadLayout = new QVBoxLayout(discussions);
    m_comments = new QTreeWidget(discussions);
    m_comments->setObjectName(QStringLiteral("hostingComments"));
    m_comments->setHeaderLabels({QStringLiteral("Author"), QStringLiteral("Provider path / position"),
                                 QStringLiteral("Commit"), QStringLiteral("Thread state")});
    m_commentDetails = new QPlainTextEdit(discussions);
    m_commentDetails->setReadOnly(true);
    m_commentDetails->setObjectName(QStringLiteral("hostingCommentDetails"));
    threadLayout->addWidget(m_comments);
    threadLayout->addWidget(m_commentDetails);
    m_tabs->addTab(discussions, QStringLiteral("Existing comments"));
    layout->addWidget(m_tabs, 2);
    m_lineSide = new QComboBox(this);
    m_lineSide->setObjectName(QStringLiteral("hostingLineSide"));
    m_lineSide->addItems({QStringLiteral("New / RIGHT"), QStringLiteral("Old / LEFT")});
    layout->addWidget(m_lineSide);
    m_anchor = new QLabel(
        QStringLiteral("Load actual review lines and select a provider diff row to anchor a comment."), this);
    m_anchor->setObjectName(QStringLiteral("hostingAnchor"));
    m_anchor->setTextFormat(Qt::PlainText);
    m_anchor->setWordWrap(true);
    layout->addWidget(m_anchor);
    m_body->setPlaceholderText(QStringLiteral("Review/comment body to send after explicit confirmation"));
    m_body->setMaximumHeight(100);
    layout->addWidget(m_body);
    auto* actions = new QGridLayout;
    int actionIndex = 0;
    for (const auto& [label, operation] : std::vector<std::pair<QString, HostingOperation>>{
             {QStringLiteral("Check Account"), HostingOperation::Account},
             {QStringLiteral("Load Page"), HostingOperation::List},
             {QStringLiteral("Details"), HostingOperation::Details},
             {QStringLiteral("Diff"), HostingOperation::Diff},
             {QStringLiteral("Load Review Lines"), HostingOperation::Files},
             {QStringLiteral("Refresh Comments"), HostingOperation::Comments},
             {QStringLiteral("Send Line Comment..."), HostingOperation::InlineComment},
             {QStringLiteral("Reply to Thread..."), HostingOperation::Reply},
             {QStringLiteral("Checkout..."), HostingOperation::Checkout},
             {QStringLiteral("Comment..."), HostingOperation::Comment},
             {QStringLiteral("Approve..."), HostingOperation::Approve},
             {QStringLiteral("Request Changes..."), HostingOperation::RequestChanges}}) {
        auto* button = new QPushButton(label, this);
        actions->addWidget(button, actionIndex / 4, actionIndex % 4);
        ++actionIndex;
        m_buttons.emplace_back(button, operation);
        connect(button, &QPushButton::clicked, this, [this, operation] { start(operation); });
    }
    layout->addLayout(actions);
    layout->addWidget(m_cancel);
    layout->addWidget(m_status);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setText(QStringLiteral("No provider request has run."));
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_worker.joinable())
            m_worker.request_stop();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QPointer<HostingPanel> owner(this);
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose installed provider CLI"));
        if (owner && !path.isEmpty())
            owner->m_executable->setText(path);
    });
    connect(m_reviews, &QTreeWidget::currentItemChanged, this, [this] { reviewSelectionChanged(); });
    connect(m_lines, &QTreeWidget::currentItemChanged, this, [this] { anchorChanged(); });
    connect(m_lineSide, &QComboBox::currentIndexChanged, this, [this] { anchorChanged(); });
    connect(m_comments, &QTreeWidget::currentItemChanged, this, [this] {
        ++m_epoch;
        if (m_current)
            m_current->store(false);
        if (m_worker.joinable())
            m_worker.request_stop();
        m_commentDetails->clear();
        if (auto* item = m_comments->currentItem(); item && m_commentRows) {
            const auto index = item->data(0, Qt::UserRole).toULongLong();
            if (index < m_commentRows->comments.size())
                m_commentDetails->setPlainText(QString::fromStdString(m_commentRows->comments[index].body));
        }
        refreshButtons();
    });
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this](int index) {
        invalidate();
        m_host->setText(index == 0 ? QStringLiteral("github.com") : QStringLiteral("gitlab.com"));
    });
    for (auto* field : {m_executable, m_gitExecutable, m_host, m_repository})
        connect(field, &QLineEdit::textChanged, this, [this] { invalidate(); });
    refreshButtons();
}
HostingPanel::~HostingPanel() {
    {
        LockGuard lock(m_gate->mutex);
        m_gate->open = false;
    }
    if (m_current)
        m_current->store(false);
    if (m_worker.joinable())
        m_worker.request_stop();
    // jthread drains the contained process before the borrowed launcher can be destroyed.
}
void HostingPanel::showTrustChanged() {
    ++m_epoch;
    clearReviewData();
    m_status->setText(QStringLiteral("Project/trust changed. An outgoing request may already have reached the "
                                     "provider; refresh comments before retrying."));
    refreshButtons();
}

void HostingPanel::invalidate() {
    ++m_epoch;
    if (m_current)
        m_current->store(false);
    if (m_worker.joinable())
        m_worker.request_stop();
    const QSignalBlocker block(m_reviews);
    m_reviews->clear();
    m_rows.clear();
    m_details->clear();
    clearReviewData();
    m_status->setText(QStringLiteral("Context changed. Load a page explicitly to refresh provider data."));
    refreshButtons();
}
void HostingPanel::clearReviewData() {
    m_diff.reset();
    m_commentRows.reset();
    const QSignalBlocker lines(m_lines), comments(m_comments);
    m_lines->clear();
    m_comments->clear();
    m_commentDetails->clear();
    m_anchor->setText(QStringLiteral("Load actual review lines and select a provider diff row to anchor a comment."));
}
void HostingPanel::reviewSelectionChanged() {
    ++m_epoch;
    if (m_current)
        m_current->store(false);
    if (m_worker.joinable())
        m_worker.request_stop();
    clearReviewData();
    m_details->clear();
    if (auto* item = m_reviews->currentItem()) {
        const auto row = item->data(0, Qt::UserRole).toULongLong();
        if (row < m_rows.size())
            m_status->setText(QStringLiteral("Selected #%1 at head %2")
                                  .arg(m_rows[row].number)
                                  .arg(QString::fromStdString(m_rows[row].head)));
    }
    refreshButtons();
}
void HostingPanel::anchorChanged() {
    ++m_epoch;
    if (m_current)
        m_current->store(false);
    if (m_worker.joinable())
        m_worker.request_stop();
    m_anchor->setText(QStringLiteral("The selected row has no inline review anchor."));
    auto* item = m_lines->currentItem();
    if (item && m_diff && item->data(0, Qt::UserRole).isValid() && item->data(0, Qt::UserRole + 1).isValid()) {
        const auto file = item->data(0, Qt::UserRole).toULongLong(),
                   line = item->data(0, Qt::UserRole + 1).toULongLong();
        if (file < m_diff->files.size() && line < m_diff->files[file].lines.size()) {
            const auto& source = m_diff->files[file];
            const auto& row = source.lines[line];
            if (!row.newLine || !row.oldLine) {
                const QSignalBlocker side(m_lineSide);
                m_lineSide->setCurrentIndex(row.newLine ? 0 : 1);
            }
            const auto coordinate = m_lineSide->currentIndex() == 0 ? row.newLine : row.oldLine;
            m_anchor->setText(
                QStringLiteral("Captured file page %1 | %2 | %3 line %4 | head %5 | base %6")
                    .arg(m_diff->page)
                    .arg(QString::fromStdString(source.newPath), m_lineSide->currentText())
                    .arg(coordinate.value_or(0))
                    .arg(QString::fromStdString(m_diff->identity.head), QString::fromStdString(m_diff->identity.base)));
        }
    }
    refreshButtons();
}
void HostingPanel::showDiff(std::shared_ptr<const HostedDiffSnapshot> diff) {
    m_diff = std::move(diff);
    const QSignalBlocker block(m_lines);
    m_lines->clear();
    for (std::size_t file = 0; file < m_diff->files.size(); ++file) {
        const auto& entry = m_diff->files[file];
        if (entry.lines.empty()) {
            new QTreeWidgetItem(
                m_lines, {QString::fromStdString(entry.newPath), {}, {}, QString::fromStdString(entry.unavailable)});
            continue;
        }
        for (std::size_t index = 0; index < entry.lines.size(); ++index) {
            const auto& line = entry.lines[index];
            auto* item = new QTreeWidgetItem(m_lines, {QString::fromStdString(entry.newPath),
                                                       line.oldLine ? QString::number(*line.oldLine) : QString{},
                                                       line.newLine ? QString::number(*line.newLine) : QString{},
                                                       QString::fromStdString(line.text)});
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(file));
            item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(index));
        }
    }
    m_tabs->setCurrentIndex(1);
    m_anchor->setText(
        QStringLiteral("Captured head %1; base %2; start %3. Select an actual patch row; "
                       "missing/truncated patches cannot be commented inline.")
            .arg(QString::fromStdString(m_diff->identity.head), QString::fromStdString(m_diff->identity.base),
                 m_diff->provider == HostingProvider::GitHub ? QStringLiteral("not used by GitHub")
                                                             : QString::fromStdString(m_diff->identity.start)));
}
void HostingPanel::showComments(std::shared_ptr<const HostedCommentPage> comments) {
    m_commentRows = std::move(comments);
    const QSignalBlocker block(m_comments);
    m_comments->clear();
    m_commentDetails->clear();
    for (std::size_t index = 0; index < m_commentRows->comments.size(); ++index) {
        const auto& comment = m_commentRows->comments[index];
        QString position = QString::fromStdString(comment.path);
        if (comment.newLine)
            position += QStringLiteral(" new:%1").arg(*comment.newLine);
        if (comment.oldLine)
            position += QStringLiteral(" old:%1").arg(*comment.oldLine);
        if (!comment.newLine && !comment.oldLine && comment.originalLine)
            position += QStringLiteral(" original:%1 (no current line returned)").arg(*comment.originalLine);
        const auto thread =
            comment.discussion.empty()
                ? QStringLiteral("Thread #%1 | ").arg(comment.replyParent ? comment.replyParent : comment.id)
                : QStringLiteral("Discussion %1 | ").arg(QString::fromStdString(comment.discussion));
        const auto state = comment.resolved
                               ? (*comment.resolved ? QStringLiteral("Resolved") : QStringLiteral("Unresolved"))
                               : QStringLiteral("State not supplied");
        auto* item = new QTreeWidgetItem(
            m_comments,
            {comment.author.empty() ? QStringLiteral("Author not supplied") : QString::fromStdString(comment.author),
             position,
             QString::fromStdString(comment.head) +
                 (comment.originalHead.empty() || comment.originalHead == comment.head
                      ? QString{}
                      : QStringLiteral(" original ") + QString::fromStdString(comment.originalHead)),
             thread + state});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(index));
    }
    m_submissionUncertain = false;
    m_tabs->setCurrentIndex(2);
}
void HostingPanel::refreshButtons() {
    const bool selected = m_reviews->currentItem() != nullptr;
    for (const auto& [button, operation] : m_buttons) {
        bool ready = selected || operation == HostingOperation::List || operation == HostingOperation::Account;
        if (operation == HostingOperation::InlineComment)
            ready = selected && m_diff && m_lines->currentItem() &&
                    m_lines->currentItem()->data(0, Qt::UserRole + 1).isValid() && !m_submissionUncertain;
        if (operation == HostingOperation::Reply)
            ready = selected && m_commentRows && m_comments->currentItem() && !m_submissionUncertain;
        button->setEnabled(!m_busy && ready &&
                           !(m_provider->currentIndex() == 1 && operation == HostingOperation::RequestChanges));
    }
    m_cancel->setEnabled(m_busy);
}
void HostingPanel::start(HostingOperation operation) {
    if (m_busy || !m_environment.launcher || !m_environment.root || !m_environment.trusted)
        return;
    HostingRequest request;
    request.provider = m_provider->currentIndex() == 0 ? HostingProvider::GitHub : HostingProvider::GitLab;
    request.operation = operation;
    request.executable = m_executable->text().toUtf8().toStdString();
    request.gitExecutable = m_gitExecutable->text().toUtf8().toStdString();
    request.root = m_environment.root();
    request.host = m_host->text().toUtf8().toStdString();
    request.repository = m_repository->text().toUtf8().toStdString();
    request.page = static_cast<std::uint32_t>(m_page->value());
    request.body = m_body->toPlainText().toUtf8().toStdString();
    if (auto* item = m_reviews->currentItem()) {
        const auto index = item->data(0, Qt::UserRole).toULongLong();
        if (index < m_rows.size()) {
            request.number = m_rows[index].number;
            request.head = m_rows[index].head;
        }
    }
    if (operation == HostingOperation::InlineComment) {
        request.diff = m_diff;
        request.right = m_lineSide->currentIndex() == 0;
        if (auto* item = m_lines->currentItem(); item && item->data(0, Qt::UserRole + 1).isValid()) {
            request.fileIndex = item->data(0, Qt::UserRole).toULongLong();
            request.lineIndex = item->data(0, Qt::UserRole + 1).toULongLong();
        } else {
            m_status->setText(QStringLiteral("Select an actual provider diff line."));
            return;
        }
    }
    if (operation == HostingOperation::Reply) {
        request.comments = m_commentRows;
        if (auto* item = m_comments->currentItem())
            request.commentIndex = item->data(0, Qt::UserRole).toULongLong();
        else
            return;
    }
    auto command = hostingCommand(request);
    if (!command) {
        m_status->setText(QString::fromStdString(command.error().message()));
        return;
    }
    if (!m_environment.trusted(request.root)) {
        m_status->setText(QStringLiteral("Trust the open project before invoking its provider CLI."));
        return;
    }
    const auto revision = m_environment.trustRevision ? m_environment.trustRevision(request.root) : 0;
    const auto epoch = m_epoch;
    const bool mutating = operation == HostingOperation::Checkout || operation == HostingOperation::Comment ||
                          operation == HostingOperation::Approve || operation == HostingOperation::RequestChanges ||
                          operation == HostingOperation::InlineComment || operation == HostingOperation::Reply;
    if (mutating) {
        QString intent =
            operation == HostingOperation::Checkout
                ? QStringLiteral("Checkout this provider branch into the current project?")
            : operation == HostingOperation::Approve
                ? QStringLiteral("Approve this captured review head on the provider?")
            : operation == HostingOperation::RequestChanges
                ? QStringLiteral("Request changes on this captured review head?")
            : operation == HostingOperation::InlineComment
                ? QStringLiteral("Send this comment on the exact reviewed provider diff line?\n") + m_anchor->text()
            : operation == HostingOperation::Reply
                ? QStringLiteral("Reply to the selected existing provider thread?\n") + m_commentDetails->toPlainText()
                : QStringLiteral("Send this comment to the provider?");
        if (operation == HostingOperation::Reply) {
            const auto& comment = request.comments->comments[request.commentIndex];
            const auto thread = comment.discussion.empty()
                                    ? std::to_string(comment.replyParent ? comment.replyParent : comment.id)
                                    : comment.discussion;
            intent =
                QStringLiteral(
                    "Reply to existing provider thread %1?\nPosition path: %2\nPosition commit: %3\nOriginal note:\n%4")
                    .arg(QString::fromStdString(thread), QString::fromStdString(comment.path),
                         comment.head.empty() ? QStringLiteral("not supplied") : QString::fromStdString(comment.head),
                         QString::fromStdString(comment.body));
        }
        QMessageBox confirm(
            QMessageBox::Question, QStringLiteral("Confirm Provider Action"),
            QStringLiteral("%1\nHost: %2\nRepository: %3\nProject: %4\nReview: #%5\nCaptured head: %6\n\n%7")
                .arg(intent)
                .arg(QString::fromStdString(request.host), QString::fromStdString(request.repository),
                     QString::fromStdString(request.root))
                .arg(request.number)
                .arg(QString::fromStdString(request.head), QString::fromStdString(request.body)),
            QMessageBox::Yes | QMessageBox::Cancel, nullptr);
        connect(this, &QObject::destroyed, &confirm, &QDialog::reject);
        confirm.setTextFormat(Qt::PlainText);
        confirm.setDefaultButton(QMessageBox::Cancel);
        const QPointer<HostingPanel> owner(this);
        if (confirm.exec() != QMessageBox::Yes || !owner)
            return;
        if (epoch != m_epoch || request.root != m_environment.root() || !m_environment.trusted(request.root) ||
            (m_environment.trustRevision && m_environment.trustRevision(request.root) != revision))
            return;
    }
    if (operation == HostingOperation::InlineComment || operation == HostingOperation::Reply)
        request.dispatched = std::make_shared<std::atomic_bool>(false);
    if (m_current)
        m_current->store(false);
    m_current = std::make_shared<std::atomic_bool>(true);
    const auto current = m_current;
    auto* launcher = m_environment.launcher;
    const auto trusted = m_environment.trusted;
    const auto trustRevision = m_environment.trustRevision;
    const QPointer<HostingPanel> owner(this);
    m_busy = true;
    refreshButtons();
    m_status->setText(QStringLiteral("Waiting for provider CLI..."));
    auto* watch = new QTimer(this);
    watch->setInterval(20);
    connect(watch, &QTimer::timeout, this, [this, current, root = request.root, revision, watch] {
        if (!current->load()) {
            watch->stop();
            watch->deleteLater();
            return;
        }
        if (!m_environment.root || m_environment.root() != root || !m_environment.trusted(root) ||
            (m_environment.trustRevision && m_environment.trustRevision(root) != revision)) {
            current->store(false);
            if (m_worker.joinable())
                m_worker.request_stop();
            showTrustChanged();
            watch->stop();
            watch->deleteLater();
        }
    });
    watch->start();
    const QPointer<QTimer> watcher(watch);
    const auto gate = m_gate;
    m_worker = std::jthread([owner, launcher, trusted, trustRevision, revision, request, current, gate,
                             watcher](std::stop_token stop) {
        const auto admitted = [current, trusted, trustRevision, revision, root = request.root] {
            return current->load() && trusted(root) && (!trustRevision || trustRevision(root) == revision);
        };
        std::shared_ptr<const HostedDiffSnapshot> diff;
        Result<std::string> result = [&]() -> Result<std::string> {
            if (request.operation == HostingOperation::Files) {
                auto loaded = loadHostedDiff(*launcher, request, admitted, stop);
                if (!loaded)
                    return std::unexpected(loaded.error());
                diff = std::make_shared<const HostedDiffSnapshot>(std::move(*loaded));
                return std::string{};
            }
            return runHosting(*launcher, request, admitted, stop);
        }();
        LockGuard lock(gate->mutex);
        if (!gate->open)
            return;
        QMetaObject::invokeMethod(
            owner.data(),
            [owner, request, current, revision, watcher, diff, result = std::move(result)]() mutable {
                if (watcher) {
                    watcher->stop();
                    watcher->deleteLater();
                }
                if (!owner)
                    return;
                // Submission knowledge outlives the selected row/epoch. Every dispatched POST
                // requires a real comment refresh before another send, even when its view vanished.
                if (request.dispatched && request.dispatched->load()) {
                    owner->m_submissionUncertain = true;
                    if (!current->load() && owner->m_environment.root && owner->m_environment.root() == request.root)
                        owner->m_status->setText(QStringLiteral("Comment outcome belongs to an earlier view; refresh "
                                                                "provider comments before sending again."));
                }
                owner->m_busy = false;
                owner->refreshButtons();
                if (!current->load() || !owner->m_environment.root || owner->m_environment.root() != request.root)
                    return;
                if (!owner->m_environment.trusted(request.root) ||
                    (owner->m_environment.trustRevision &&
                     owner->m_environment.trustRevision(request.root) != revision)) {
                    // The worker can finish before the timer gets its next tick.
                    current->store(false);
                    owner->showTrustChanged();
                    return;
                }
                if (!result) {
                    owner->m_status->setText(QString::fromStdString(result.error().message()));

                    owner->refreshButtons();
                    return;
                }
                if (request.operation == HostingOperation::Files) {
                    owner->showDiff(std::move(diff));
                    owner->m_status->setText(QStringLiteral("Loaded actual provider file page %1. Comments pin the "
                                                            "captured head/base; reload on stale-head refusal.")
                                                 .arg(request.page));
                    owner->refreshButtons();
                } else if (request.operation == HostingOperation::Comments) {
                    auto comments = parseHostedComments(request, *result);
                    if (!comments) {
                        owner->m_status->setText(QString::fromStdString(comments.error().message()));
                        return;
                    }
                    owner->showComments(std::make_shared<const HostedCommentPage>(std::move(*comments)));
                    owner->m_status->setText(QStringLiteral("Loaded actual provider comment/thread page %1; original "
                                                            "positions and missing current anchors remain explicit.")
                                                 .arg(request.page));
                    owner->refreshButtons();
                } else if (request.operation == HostingOperation::List) {
                    auto rows = parseHostedReviews(request.provider, *result);
                    const QSignalBlocker selected(owner->m_reviews);
                    owner->m_reviews->clear();
                    owner->m_rows.clear();
                    owner->clearReviewData();
                    if (!rows) {
                        owner->m_status->setText(QString::fromStdString(rows.error().message()));
                        return;
                    }
                    owner->m_rows = std::move(*rows);
                    for (std::size_t i = 0; i < owner->m_rows.size(); ++i) {
                        const auto& row = owner->m_rows[i];
                        auto* item = new QTreeWidgetItem(
                            owner->m_reviews, {QString::number(row.number), QString::fromStdString(row.title),
                                               QString::fromStdString(row.author),
                                               QString::fromStdString(row.source + " → " + row.target),
                                               QString::fromStdString(row.state)});
                        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(i));
                    }
                    owner->m_status->setText(
                        QStringLiteral("Loaded %1 provider rows on page %2; use the page field for older results.")
                            .arg(owner->m_rows.size())
                            .arg(request.page));
                } else if (request.operation == HostingOperation::Account) {
                    const auto identity = QJsonDocument::fromJson(QByteArray::fromStdString(*result)).object();
                    const auto key = request.provider == HostingProvider::GitHub ? QStringLiteral("login")
                                                                                 : QStringLiteral("username");
                    if (!identity.value(key).isString()) {
                        owner->m_status->setText(
                            QStringLiteral("Provider returned no authenticated account identity."));
                        return;
                    }
                    owner->m_status->setText(
                        QStringLiteral("Authenticated account: %1 @ %2")
                            .arg(identity.value(key).toString(), QString::fromStdString(request.host)));
                } else {
                    owner->m_details->setPlainText(
                        QString::fromUtf8(result->data(), static_cast<qsizetype>(result->size())));
                    owner->m_status->setText(QStringLiteral("Provider operation completed for #%1 (captured head %2).")
                                                 .arg(request.number)
                                                 .arg(QString::fromStdString(request.head)));
                    if (request.operation == HostingOperation::Comment ||
                        request.operation == HostingOperation::Approve ||
                        request.operation == HostingOperation::RequestChanges ||
                        request.operation == HostingOperation::InlineComment ||
                        request.operation == HostingOperation::Reply)
                        owner->m_body->clear();
                    if (request.operation == HostingOperation::InlineComment ||
                        request.operation == HostingOperation::Reply) {
                        owner->m_status->setText(
                            QStringLiteral("Provider confirmed the captured comment/thread. Refreshing actual "
                                           "comments; a concurrent new head does not move its original anchor."));
                        owner->m_page->setValue(1);
                        owner->start(HostingOperation::Comments);
                    }
                }
            },
            Qt::QueuedConnection);
    });
}
} // namespace frost::app
