// SPDX-License-Identifier: Apache-2.0
// Genuine GitHub/GitLab review pages, account identity, metadata/diffs, checkout and explicitly confirmed reviews.
#include "hosting_panel.hpp"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
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
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace frost::app {
HostingPanel::HostingPanel(HostingEnvironment environment, QWidget* parent)
    : QWidget(parent), m_environment(std::move(environment)), m_provider(new QComboBox(this)),
      m_executable(new QLineEdit(this)), m_gitExecutable(new QLineEdit(this)), m_host(new QLineEdit(QStringLiteral("github.com"), this)),
      m_repository(new QLineEdit(this)), m_page(new QSpinBox(this)), m_reviews(new QTreeWidget(this)),
      m_details(new QPlainTextEdit(this)), m_body(new QPlainTextEdit(this)), m_status(new QLabel(this)),
      m_cancel(new QPushButton(QStringLiteral("Cancel Request"), this)) {
    setObjectName(QStringLiteral("hostingPanel"));
    m_executable->setObjectName(QStringLiteral("hostingExecutable"));
    m_host->setObjectName(QStringLiteral("hostingHost"));
    m_repository->setObjectName(QStringLiteral("hostingRepository"));
    m_reviews->setObjectName(QStringLiteral("hostingReviews"));
    m_body->setObjectName(QStringLiteral("hostingReviewBody"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Use your separately installed and authenticated GitHub or GitLab CLI. FrostIDE never reads provider credentials. Requests run only when invoked below."), this));
    auto* form = new QFormLayout;
    m_provider->addItems({QStringLiteral("GitHub (gh)"), QStringLiteral("GitLab (glab)")});
    form->addRow(QStringLiteral("Provider"), m_provider);
    auto* path = new QWidget(this);
    auto* pathLayout = new QHBoxLayout(path); pathLayout->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton(QStringLiteral("Choose CLI..."), path);
    pathLayout->addWidget(m_executable); pathLayout->addWidget(browse);
    form->addRow(QStringLiteral("CLI executable"), path);
    m_gitExecutable->setObjectName(QStringLiteral("hostingGitExecutable"));
    form->addRow(QStringLiteral("Git executable (checkout verification)"), m_gitExecutable);
    form->addRow(QStringLiteral("Host"), m_host);
    form->addRow(QStringLiteral("Owner / repository"), m_repository);
    m_page->setRange(1, 10000);
    form->addRow(QStringLiteral("Page (100 reviews)"), m_page);
    layout->addLayout(form);
    m_reviews->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Author"), QStringLiteral("Source → Target"), QStringLiteral("State")});
    m_reviews->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_reviews, 1);
    m_details->setReadOnly(true); m_details->setObjectName(QStringLiteral("hostingDetails"));
    layout->addWidget(m_details, 1);
    m_body->setPlaceholderText(QStringLiteral("Review/comment body to send after explicit confirmation"));
    m_body->setMaximumHeight(100); layout->addWidget(m_body);
    auto* actions = new QHBoxLayout;
    for (const auto& [label, operation] : std::vector<std::pair<QString, HostingOperation>>{
        {QStringLiteral("Check Account"), HostingOperation::Account}, {QStringLiteral("Load Page"), HostingOperation::List},
        {QStringLiteral("Details"), HostingOperation::Details}, {QStringLiteral("Diff"), HostingOperation::Diff},
        {QStringLiteral("Checkout..."), HostingOperation::Checkout}, {QStringLiteral("Comment..."), HostingOperation::Comment},
        {QStringLiteral("Approve..."), HostingOperation::Approve}, {QStringLiteral("Request Changes..."), HostingOperation::RequestChanges}}) {
        auto* button = new QPushButton(label, this);
        actions->addWidget(button); m_buttons.emplace_back(button, operation);
        connect(button, &QPushButton::clicked, this, [this, operation] { start(operation); });
    }
    layout->addLayout(actions); layout->addWidget(m_cancel); layout->addWidget(m_status);
    m_status->setWordWrap(true); m_status->setText(QStringLiteral("No provider request has run."));
    connect(m_cancel, &QPushButton::clicked, this, [this] { if (m_worker.joinable()) m_worker.request_stop(); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QPointer<HostingPanel> owner(this);
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose installed provider CLI"));
        if (owner && !path.isEmpty()) owner->m_executable->setText(path);
    });
    connect(m_reviews, &QTreeWidget::currentItemChanged, this, [this] {
        m_details->clear();
        if (auto* item = m_reviews->currentItem()) {
            const auto row = item->data(0, Qt::UserRole).toULongLong();
            if (row < m_rows.size()) m_status->setText(QStringLiteral("Selected #%1 at head %2").arg(m_rows[row].number).arg(QString::fromStdString(m_rows[row].head)));
        }
        refreshButtons();
    });
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this](int index) {
        invalidate(); m_host->setText(index == 0 ? QStringLiteral("github.com") : QStringLiteral("gitlab.com"));
    });
    for (auto* field : {m_executable, m_gitExecutable, m_host, m_repository}) connect(field, &QLineEdit::textChanged, this, [this] { invalidate(); });
    refreshButtons();
}
HostingPanel::~HostingPanel() {
    { LockGuard lock(m_gate->mutex); m_gate->open = false; }
    if (m_current) m_current->store(false);
    if (m_worker.joinable()) m_worker.request_stop();
    // jthread drains the contained process before the borrowed launcher can be destroyed.
}
void HostingPanel::invalidate() {
    ++m_epoch;
    if (m_current) m_current->store(false);
    if (m_worker.joinable()) m_worker.request_stop();
    m_reviews->clear(); m_rows.clear(); m_details->clear();
    m_status->setText(QStringLiteral("Context changed. Load a page explicitly to refresh provider data."));
    refreshButtons();
}
void HostingPanel::refreshButtons() {
    const bool selected = m_reviews->currentItem() != nullptr;
    for (const auto& [button, operation] : m_buttons)
        button->setEnabled(!m_busy && (selected || operation == HostingOperation::List || operation == HostingOperation::Account) && !(m_provider->currentIndex() == 1 && operation == HostingOperation::RequestChanges));
    m_cancel->setEnabled(m_busy);
}
void HostingPanel::start(HostingOperation operation) {
    if (m_busy || !m_environment.launcher || !m_environment.root || !m_environment.trusted) return;
    HostingRequest request;
    request.provider = m_provider->currentIndex() == 0 ? HostingProvider::GitHub : HostingProvider::GitLab;
    request.operation = operation; request.executable = m_executable->text().toUtf8().toStdString();
    request.gitExecutable = m_gitExecutable->text().toUtf8().toStdString();
    request.root = m_environment.root(); request.host = m_host->text().toUtf8().toStdString();
    request.repository = m_repository->text().toUtf8().toStdString(); request.page = static_cast<std::uint32_t>(m_page->value());
    request.body = m_body->toPlainText().toUtf8().toStdString();
    if (auto* item = m_reviews->currentItem()) {
        const auto index = item->data(0, Qt::UserRole).toULongLong();
        if (index < m_rows.size()) { request.number = m_rows[index].number; request.head = m_rows[index].head; }
    }
    auto command = hostingCommand(request);
    if (!command) { m_status->setText(QString::fromStdString(command.error().message())); return; }
    if (!m_environment.trusted(request.root)) { m_status->setText(QStringLiteral("Trust the open project before invoking its provider CLI.")); return; }
    const auto epoch = m_epoch;
    const bool mutating = operation == HostingOperation::Checkout || operation == HostingOperation::Comment || operation == HostingOperation::Approve || operation == HostingOperation::RequestChanges;
    if (mutating) {
        QMessageBox confirm(QMessageBox::Question, QStringLiteral("Confirm Provider Action"),
            QStringLiteral("%1\nHost: %2\nRepository: %3\nProject: %4\nReview: #%5\nCaptured head: %6\n\n%7")
                .arg(operation == HostingOperation::Checkout ? QStringLiteral("Checkout this provider branch into the current project?") : operation == HostingOperation::Approve ? QStringLiteral("Approve this captured review head on the provider?") : operation == HostingOperation::RequestChanges ? QStringLiteral("Request changes on this captured review head?") : QStringLiteral("Send this comment to the provider?"))
                .arg(QString::fromStdString(request.host), QString::fromStdString(request.repository), QString::fromStdString(request.root))
                .arg(request.number).arg(QString::fromStdString(request.head), QString::fromStdString(request.body)),
            QMessageBox::Yes | QMessageBox::Cancel, nullptr);
        connect(this, &QObject::destroyed, &confirm, &QDialog::reject);
        confirm.setTextFormat(Qt::PlainText); confirm.setDefaultButton(QMessageBox::Cancel);
        const QPointer<HostingPanel> owner(this);
        if (confirm.exec() != QMessageBox::Yes || !owner) return;
        if (epoch != m_epoch || request.root != m_environment.root() || !m_environment.trusted(request.root)) return;
    }
    if (m_current) m_current->store(false);
    m_current = std::make_shared<std::atomic_bool>(true);
    const auto current = m_current;
    auto* launcher = m_environment.launcher;
    const auto trusted = m_environment.trusted;
    const QPointer<HostingPanel> owner(this);
    m_busy = true; refreshButtons(); m_status->setText(QStringLiteral("Waiting for provider CLI..."));
    const auto gate = m_gate;
    m_worker = std::jthread([owner, launcher, trusted, request, current, gate](std::stop_token stop) {
        auto result = runHosting(*launcher, request, [current, trusted, root = request.root] { return current->load() && trusted(root); }, stop);
        LockGuard lock(gate->mutex);
        if (!gate->open) return;
        QMetaObject::invokeMethod(owner.data(), [owner, request, current, result = std::move(result)]() mutable {
            if (!owner) return;
            owner->m_busy = false; owner->refreshButtons();
            if (!current->load() || !owner->m_environment.root || owner->m_environment.root() != request.root) return;
            if (!result) { owner->m_status->setText(QString::fromStdString(result.error().message())); return; }
            if (request.operation == HostingOperation::List) {
                auto rows = parseHostedReviews(request.provider, *result);
                owner->m_reviews->clear(); owner->m_rows.clear();
                if (!rows) { owner->m_status->setText(QString::fromStdString(rows.error().message())); return; }
                owner->m_rows = std::move(*rows);
                for (std::size_t i = 0; i < owner->m_rows.size(); ++i) {
                    const auto& row = owner->m_rows[i];
                    auto* item = new QTreeWidgetItem(owner->m_reviews, {QString::number(row.number), QString::fromStdString(row.title), QString::fromStdString(row.author), QString::fromStdString(row.source + " → " + row.target), QString::fromStdString(row.state)});
                    item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(i));
                }
                owner->m_status->setText(QStringLiteral("Loaded %1 provider rows on page %2; use the page field for older results.").arg(owner->m_rows.size()).arg(request.page));
            } else if (request.operation == HostingOperation::Account) {
                const auto identity = QJsonDocument::fromJson(QByteArray::fromStdString(*result)).object();
                const auto key = request.provider == HostingProvider::GitHub ? QStringLiteral("login") : QStringLiteral("username");
                if (!identity.value(key).isString()) { owner->m_status->setText(QStringLiteral("Provider returned no authenticated account identity.")); return; }
                owner->m_status->setText(QStringLiteral("Authenticated account: %1 @ %2").arg(identity.value(key).toString(), QString::fromStdString(request.host)));
            } else {
                owner->m_details->setPlainText(QString::fromUtf8(result->data(), static_cast<qsizetype>(result->size())));
                owner->m_status->setText(QStringLiteral("Provider operation completed for #%1 (captured head %2).").arg(request.number).arg(QString::fromStdString(request.head)));
                if (request.operation == HostingOperation::Comment || request.operation == HostingOperation::Approve || request.operation == HostingOperation::RequestChanges) owner->m_body->clear();
            }
        }, Qt::QueuedConnection);
    });
}
}
