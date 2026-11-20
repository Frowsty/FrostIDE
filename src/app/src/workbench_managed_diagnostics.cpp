// SPDX-License-Identifier: Apache-2.0
// Explicit managed diagnostics for an actual IDE-owned run; the collector never stops its target.
#include "frost/app/workbench.hpp"
#include "frost/debug/coreclr_dap.hpp"
#include "frost/run/managed_diagnostics.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <thread>
namespace frost::app {
void Workbench::showManagedDiagnostics() {
    if (!m_window || m_shutDown || !m_runManager)
        return;
    const auto model = managedProjectEvaluation();
    const auto configurations = managedRunConfigurations();
    if (!model || configurations.empty()) {
        QMessageBox::information(m_window, tr(".NET Diagnostics"),
                                 tr("Choose an evaluated SDK project and start its .NET Run configuration first."));
        return;
    }
    std::vector<run::RunStatus> targets;
    for (const auto& status : m_runManager->activeRuns())
        if (status.phase == run::RunPhase::Running && status.processId &&
            std::ranges::find(configurations, status.configuration) != configurations.end())
            targets.push_back(status);
    QDialog dialog(m_window);
    dialog.setObjectName(QStringLiteral("managedDiagnostics"));
    dialog.setWindowTitle(tr(".NET Diagnostics"));
    dialog.resize(1000, 720);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* process = new QComboBox(&dialog);
    process->setObjectName(QStringLiteral("managedDiagnosticTarget"));
    for (const auto& target : targets)
        process->addItem(QString::fromStdString(target.configuration.name) + tr(" — PID %1").arg(target.processId));
    form->addRow(tr("Owned managed run"), process);
    auto* kind = new QComboBox(&dialog);
    kind->addItems(
        {tr("Managed thread-time trace"), tr("Runtime / ASP.NET counters"), tr("GC heap statistics (full GC)")});
    kind->setObjectName(QStringLiteral("managedDiagnosticKind"));
    form->addRow(tr("Collection"), kind);
    auto* tool = new QLineEdit(&dialog);
    tool->setObjectName(QStringLiteral("managedDiagnosticTool"));
    auto* browse = new QPushButton(tr("Choose installed tool..."), &dialog);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("Installed matching diagnostic executable"));
        if (!path.isEmpty())
            tool->setText(path);
    });
    form->addRow(tr("Absolute executable"), tool);
    form->addRow(browse);
    auto* version = new QLineEdit(&dialog);
    version->setObjectName(QStringLiteral("managedDiagnosticVersion"));
    form->addRow(tr("Exact installed tool version"), version);
    auto* seconds = new QSpinBox(&dialog);
    seconds->setRange(1, 300);
    seconds->setValue(10);
    form->addRow(tr("Seconds / GC timeout"), seconds);
    auto* directory = new QLineEdit(&dialog);
    directory->setObjectName(QStringLiteral("managedDiagnosticOutput"));
    form->addRow(tr("New capture directory"), directory);
    layout->addLayout(form);
    auto* status =
        new QLabel(targets.empty() ? tr("No running configuration matches the selected evaluated SDK context.")
                                   : tr("Explicit installed tools only. EventPipe permission and matching Linux TMPDIR "
                                        "are required. No downloads."),
                   &dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setObjectName(QStringLiteral("managedDiagnosticStatus"));
    layout->addWidget(status);
    auto* output = new QPlainTextEdit(&dialog);
    output->setReadOnly(true);
    output->setObjectName(QStringLiteral("managedDiagnosticReport"));
    layout->addWidget(output);
    auto* collect = new QPushButton(tr("Collect"), &dialog);
    auto* stop = new QPushButton(tr("Cancel collector"), &dialog);
    stop->setEnabled(false);
    collect->setEnabled(!targets.empty());
    layout->addWidget(collect);
    layout->addWidget(stop);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::jthread worker;
    std::stop_source operation;
    auto lease = std::make_shared<std::atomic_bool>(true);
    const auto root = m_projectRoot;
    const auto trustRevision = m_services.trust.snapshot(root).revision;
    const auto current = [&] {
        if (m_shutDown || root != m_projectRoot || managedProjectEvaluation() != model)
            return false;
        const auto trust = m_services.trust.snapshot(root);
        return trust.trusted && trust.revision == trustRevision;
    };
    auto revoke = [&] {
        lease->store(false);
        operation.request_stop();
    };
    connect(this, &Workbench::projectChanged, &dialog, revoke);
    connect(this, &Workbench::managedProjectChanged, &dialog, revoke);
    connect(stop, &QPushButton::clicked, &dialog, [&] { operation.request_stop(); });
    QTimer timer(&dialog);
    timer.setInterval(25);
    std::optional<run::RunId> selected;
    connect(&timer, &QTimer::timeout, &dialog, [&] {
        bool valid = current();
        if (selected && valid && m_runManager) {
            const auto now = m_runManager->status(*selected);
            valid = valid && now && now->phase == run::RunPhase::Running;
        }
        if (!valid)
            revoke();
    });
    timer.start();
    connect(collect, &QPushButton::clicked, &dialog, [&] {
        if (!current() || !lease->load() || process->currentIndex() < 0)
            return;
        const auto target = targets[static_cast<std::size_t>(process->currentIndex())];
        const auto now = m_runManager->status(target.id);
        if (!now || now->processId != target.processId || now->configuration != target.configuration)
            return;
        bool fullGc = kind->currentIndex() == 2;
        if (fullGc && QMessageBox::warning(&dialog, tr("Full GC collection"),
                                           tr("GC dump triggers a full generation2 garbage collection and may suspend "
                                              "this process for a long time. Collect now?"),
                                           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        const auto afterConsent = m_runManager->status(target.id);
        if (!current() || !lease->load() || !afterConsent || afterConsent->processId != target.processId ||
            afterConsent->configuration != target.configuration || afterConsent->phase != run::RunPhase::Running)
            return;
        if (worker.joinable())
            worker.join();
        operation = std::stop_source();
        selected = target.id;
        run::ManagedDiagnosticRequest request;
        request.kind = static_cast<run::ManagedDiagnosticKind>(kind->currentIndex());
        request.tool = tool->text().toStdString();
        request.expectedVersion = version->text().toStdString();
        request.outputDirectory = directory->text().toStdString();
        request.workingDirectory = target.configuration.workingDirectory;
        request.processId = static_cast<std::uint32_t>(target.processId);
        request.durationSeconds = static_cast<unsigned>(seconds->value());
        request.allowFullGc = fullGc;
        for (const auto& env : target.configuration.environment)
            request.environment.emplace_back(env.name, env.value);
        auto admission = managedRunAdmissionFor(target.configuration);
        auto* fs = &m_services.fs;
        auto* launcher = &m_services.launcher;
        auto* trust = &m_services.trust;
        auto token = operation.get_token();
        collect->setEnabled(false);
        stop->setEnabled(true);
        status->setText(tr("Collecting actual tool output; Cancel ends the collector only."));
        worker = std::jthread([&, request = std::move(request), admission, fs, launcher, trust, token, target,
                               lease](std::stop_token) mutable {
            auto result = [&]() -> Result<run::ManagedDiagnosticReport> {
                if (auto allowed = admission(); !allowed)
                    return std::unexpected(allowed.error());
                auto assembly = run::captureAnalysisIdentity(*fs, target.configuration.arguments.front());
                if (!assembly)
                    return std::unexpected(assembly.error());
                auto processIdentity = debug::coreClrProcessAdmission(request.processId, target.configuration.program);
                if (!processIdentity)
                    return std::unexpected(processIdentity.error());
                request.admit = [lease, trust, root, trustRevision, identity = *processIdentity]() -> Result<void> {
                    const auto currentTrust = trust->snapshot(root);
                    if (!lease->load() || !currentTrust.trusted || currentTrust.revision != trustRevision)
                        return fail(ErrorCode::Cancelled, "Managed diagnostic project/context/trust changed");
                    return identity();
                };
                auto report = run::collectManagedDiagnostics(*fs, *launcher, request, token);
                if (!report)
                    return report;
                if (auto allowed = admission(); !allowed)
                    return std::unexpected(allowed.error());
                if (auto valid = run::validateAnalysisIdentity(*fs, *assembly); !valid)
                    return std::unexpected(valid.error());
                return report;
            }();
            QMetaObject::invokeMethod(
                &dialog,
                [&, result = std::move(result), token, lease]() mutable {
                    stop->setEnabled(false);
                    collect->setEnabled(current() && lease->load());
                    if (token.stop_requested() || !current() || !lease->load()) {
                        status->setText(tr("Collection cancelled or SDK context changed; no result published."));
                        return;
                    }
                    if (!result) {
                        status->setText(QString::fromStdString(result.error().message()));
                        return;
                    }
                    status->setText(tr("Captured snapshot — PID %1; %2")
                                        .arg(result->processId)
                                        .arg(QString::fromStdString(result->artifact.path)));
                    output->setPlainText(QString::fromStdString(result->report));
                },
                Qt::QueuedConnection);
        });
    });
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
    revoke();
    if (worker.joinable())
        worker.join();
}
} // namespace frost::app
