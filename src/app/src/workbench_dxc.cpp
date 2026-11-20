// SPDX-License-Identifier: Apache-2.0
#include "frost/app/workbench.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "dxc_profile.hpp"
#include "trusted_project_launcher.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <map>
namespace frost::app {
namespace {
QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
using Images = std::map<std::string, std::string>;
using Done = std::function<void(Result<DxcReport>)>;
sched::Task<void> diagnose(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                           DxcProfile profile, uint64_t revision, Images images, std::stop_token stop, Done done) {
    auto source = fs->readFileSnapshot(profile.source, 4 * 1024 * 1024);
    if (!source) {
        done(std::unexpected(source.error()));
        co_return;
    }
    for (const auto& [path, bytes] : images) {
        auto saved = fs->readFileSnapshot(path, 4 * 1024 * 1024);
        if (!saved || *saved != bytes) {
            done(fail(ErrorCode::Busy, "Save unchanged UTF-8 HLSL documents before DXC diagnostics"));
            co_return;
        }
    }
    TrustedProjectLauncher admitted(*launcher, *trust, profile.root, revision, stop);
    auto report = runDxc(admitted, profile, stop);
    auto current = fs->readFileSnapshot(profile.source, 4 * 1024 * 1024);
    if (!current || *current != *source) {
        done(fail(ErrorCode::Busy, "HLSL source changed during DXC; diagnostics discarded"));
        co_return;
    }
    for (const auto& [path, bytes] : images) {
        auto saved = fs->readFileSnapshot(path, 4 * 1024 * 1024);
        if (!saved || *saved != bytes) {
            done(fail(ErrorCode::Busy, "Saved HLSL input changed during DXC; diagnostics discarded"));
            co_return;
        }
    }
    done(std::move(report));
}
} // namespace
void Workbench::showDxcDiagnostics() {
    if (!m_window || m_shutDown || !m_pool || m_projectRoot.empty())
        return;
    const auto admission = m_services.trust.snapshot(m_projectRoot);
    if (!admission.trusted) {
        offerProjectTrust();
        return;
    }
    QPointer<Workbench> lifetime(this);
    const auto shell = m_window;
    const auto projectRoot = m_projectRoot;
    const auto projectGeneration = m_projectEditAdmissionGeneration;
    const auto operationGeneration = m_dxcGeneration;
    const auto admitted = [this, lifetime, shell, projectRoot, projectGeneration, operationGeneration, admission] {
        if (!lifetime || !shell || m_shutDown || m_projectRoot != projectRoot ||
            m_projectEditAdmissionGeneration != projectGeneration || m_dxcGeneration != operationGeneration)
            return false;
        const auto current = m_services.trust.snapshot(projectRoot);
        return current.trusted && current.revision == admission.revision;
    };
    // A shell-owned QObject must never delete this stack object during a nested event loop.
    QDialog dialog;
    dialog.setWindowModality(Qt::ApplicationModal);
    connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    connect(shell.data(), &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setObjectName("dxcDiagnosticsDialog");
    dialog.setWindowTitle(tr("DXC HLSL Diagnostics"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(tr("Compile one saved standalone HLSL entry with your installed DXC. ShaderLab generated "
                               "variants, Unity keywords and Unity include mappings are not inferred."),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* executable = new QLineEdit(q(m_dxcExecutable), &dialog);
    executable->setObjectName("dxcExecutable");
    form->addRow(tr("Installed DXC"), executable);
    auto* browse = new QPushButton(tr("Choose DXC..."), &dialog);
    form->addRow(browse);
    browse->setObjectName("dxcBrowse");
    connect(browse, &QPushButton::clicked, &dialog,
            [lifetime, shell, owner = QPointer<QDialog>(&dialog), field = QPointer<QLineEdit>(executable)] {
                if (!lifetime || !shell || !owner || !field)
                    return;
                QFileDialog picker;
                picker.setObjectName("dxcCompilerPicker");
                picker.setWindowTitle(tr("Select installed DXC"));
                picker.setFileMode(QFileDialog::ExistingFile);
                picker.setWindowModality(Qt::ApplicationModal);
                connect(lifetime.data(), &QObject::destroyed, &picker, &QDialog::reject);
                connect(shell.data(), &QObject::destroyed, &picker, &QDialog::reject);
                connect(owner.data(), &QDialog::finished, &picker, &QDialog::reject);
                const auto accepted = picker.exec() == QDialog::Accepted;
                if (!lifetime || !shell || !owner || !field || !owner->isVisible() || !accepted)
                    return;
                const auto files = picker.selectedFiles();
                if (!files.isEmpty())
                    field->setText(files.front());
            });
    auto* source = new QLineEdit(&dialog);
    if (auto* view = currentView())
        source->setText(q(view->path()));
    source->setObjectName("dxcSource");
    form->addRow(tr("Saved .hlsl source"), source);
    auto* entry = new QLineEdit(q(m_dxcEntry.empty() ? "main" : m_dxcEntry), &dialog);
    entry->setObjectName("dxcEntry");
    form->addRow(tr("Entry point"), entry);
    auto* target = new QLineEdit(q(m_dxcTarget.empty() ? "ps_6_0" : m_dxcTarget), &dialog);
    target->setObjectName("dxcTarget");
    form->addRow(tr("Target"), target);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !admitted())
        return;
    DxcProfile profile{executable->text().toStdString(), projectRoot, source->text().toStdString(),
                       entry->text().toStdString(), target->text().toStdString()};
    auto spec = dxcSpec(profile);
    if (!spec) {
        notify(tr("DXC unavailable"), q(spec.error().message()), ui::NotificationKind::Warning);
        return;
    }
    const QFileInfo tool(q(profile.executable)), file(q(profile.source));
    const QString physicalRoot = QFileInfo(q(profile.root)).canonicalFilePath();
    const QString physicalSource = file.canonicalFilePath();
    const auto relative = QDir(physicalRoot).relativeFilePath(physicalSource);
    if (!tool.isFile() || !file.isFile() || physicalRoot.isEmpty() || physicalSource.isEmpty() ||
        QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) {
        notify(tr("DXC unavailable"),
               tr("Select an installed compiler and an existing HLSL source inside the trusted project."),
               ui::NotificationKind::Warning);
        return;
    }
    auto saved = saveAllDocuments();
    if (!admitted())
        return;
    if (!saved.failed.empty() || !saved.skipped.empty()) {
        notify(tr("DXC unavailable"), tr("Save All must complete before compiling saved HLSL."),
               ui::NotificationKind::Warning);
        return;
    }
    Images images;
    size_t total = 0;
    for (const auto& document : m_documents->documents())
        if (document->path().ends_with(".hlsl") || document->path().ends_with(".hlsli")) {
            auto bytes = document->document().text().text();
            total += bytes.size();
            if (total > 16 * 1024 * 1024) {
                notify(tr("DXC unavailable"), tr("Open HLSL snapshot exceeds 16 MiB."), ui::NotificationKind::Warning);
                return;
            }
            images.emplace(document->path(), std::move(bytes));
        }
    if (!admitted())
        return;
    const auto permission = admission;
    m_dxcExecutable = profile.executable;
    m_dxcEntry = profile.entry;
    m_dxcTarget = profile.target;
    m_dxcStop.request_stop();
    m_dxcStop = std::stop_source{};
    const auto stop = m_dxcStop.get_token();
    const auto generation = ++m_dxcGeneration;
    const auto editGeneration = m_projectEditAdmissionGeneration;
    m_dxcTrustRevision = permission.revision;
    QPointer<Workbench> self(this);
    Done done = [self, profile, images, generation, editGeneration,
                 revision = permission.revision](Result<DxcReport> report) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, profile, images, generation, editGeneration, revision, report = std::move(report)]() mutable {
                if (!self || self->m_shutDown || !self->m_window || self->m_dxcGeneration != generation ||
                    self->m_projectRoot != profile.root || self->m_projectEditAdmissionGeneration != editGeneration)
                    return;
                const auto trust = self->m_services.trust.snapshot(profile.root);
                if (!trust.trusted || trust.revision != revision)
                    return;
                Images current;
                for (const auto& document : self->m_documents->documents())
                    if (document->path().ends_with(".hlsl") || document->path().ends_with(".hlsli"))
                        current.emplace(document->path(), document->document().text().text());
                if (current != images) {
                    self->notify(tr("DXC result discarded"), tr("Open HLSL documents changed during compilation."),
                                 ui::NotificationKind::Warning);
                    return;
                }
                if (!report) {
                    self->notify(tr("DXC failed"), q(report.error().message()), ui::NotificationKind::Warning);
                    return;
                }
                if (self->m_problems)
                    self->m_problems->model()->setProblems(ui::DiagnosticSource::Dxc, report->diagnostics);
                self->m_window->toolWindows().show(ui::tool_windows::kProblems, false);
                self->notify(tr("DXC HLSL Diagnostics"),
                             tr("DXC exited %1; %2 compiler diagnostics. Entry %3, target %4.")
                                 .arg(report->exitCode)
                                 .arg(static_cast<qulonglong>(report->diagnostics.size()))
                                 .arg(q(profile.entry), q(profile.target)),
                             report->exitCode == 0 ? ui::NotificationKind::Info : ui::NotificationKind::Warning);
                if (report->exitCode != 0 && report->diagnostics.empty())
                    self->notify(tr("DXC compiler output"), q(report->output.substr(0, 8192)),
                                 ui::NotificationKind::Warning);
            },
            Qt::QueuedConnection);
    };
    m_pool
        ->spawn(sched::Lane::Background, diagnose(&m_services.fs, &m_services.launcher, &m_services.trust, profile,
                                                  permission.revision, std::move(images), stop, std::move(done)))
        .detach();
}
} // namespace frost::app
