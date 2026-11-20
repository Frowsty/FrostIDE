// SPDX-License-Identifier: Apache-2.0
// Explicit Unity Editor/player Mono 2.58 attach through actual held source evidence and OS-owned endpoint identity.
#include "frost/app/workbench.hpp"
#include "frost/debug/mono_dap.hpp"
#include "frost/debug/mono_windows_peer.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "managed_snapshot_job.hpp"
#include "mono_source_admission.hpp"
#include "mono_adapter_preparation.hpp"
#include "unity_control.hpp"
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QSpinBox>
#include <algorithm>
namespace frost::app {
namespace {
using MonoCompletion = std::function<void(Result<MonoAdmittedModule>)>;
[[maybe_unused]] sched::Task<void> prepareUnityModule(std::shared_ptr<ManagedSnapshotJob> job, io::IProcessLauncher* launcher,
    project::TrustStore* trust, std::string root, std::uint64_t revision, UnityMonoDebugSelection selection,
    std::string held, std::string guard, std::function<bool()> identityCurrent, MonoCompletion done) {
    const auto current = [job, trust, root, revision, identityCurrent] {
        const auto state = trust->snapshot(root);
        return !job->cancelled.load() && state.trusted && state.revision == revision && identityCurrent();
    };
    auto result = admitMonoModule(*launcher, held, selection.dotnet, guard, root, selection.assembly, selection.pdb,
        {}, current);
    LockGuard lock(job->mutex);
    if (job->target && !job->cancelled.load()) QMetaObject::invokeMethod(job->target,
        [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); }, Qt::QueuedConnection);
    co_return;
}
}
void Workbench::startUnityDebug() {
    if (!m_window || !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy()) return;
#ifndef _WIN32
    notify(QStringLiteral("Unity Debugger"), QStringLiteral("The verified Mono endpoint/source profile currently requires Windows. No adapter was started."), ui::NotificationKind::Error);
    return;
#else
    const auto root = m_projectRoot;
    const auto authorized = m_services.trust.snapshot(root);
    if (!authorized.trusted) { notify(QStringLiteral("Unity Debugger"), QStringLiteral("Trust the Unity project before attaching."), ui::NotificationKind::Error); return; }
    const auto editor = m_unity ? m_unity->selectedEditor() : std::nullopt;
    std::optional<UnityMonoDebugSelection> selected;
    if (m_options.selectUnityDebug) selected = m_options.selectUnityDebug(root);
    else {
        QDialog dialog;
        const QPointer<Workbench> owner(this);
        connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
        dialog.setWindowTitle(QStringLiteral("Attach Unity Debugger"));
        auto* form = new QFormLayout(&dialog);
        auto* description = new QLabel(QStringLiteral("Verified Mono 2.58 profile: one loaded module and PDB, each below 1 MiB. Source frames use verified snapshots. Domain reload disconnects; no automatic state/breakpoint survival is claimed. IL2CPP is unavailable until a separate actual debugger contract is established."), &dialog);
        description->setWordWrap(true); form->addRow(description);
        QComboBox host(&dialog); host.addItems({QStringLiteral("Selected Unity Editor"), QStringLiteral("Running Mono Player")});
        QComboBox backend(&dialog); backend.addItems({QStringLiteral("Mono 2.58 verified profile"), QStringLiteral("IL2CPP (unsupported)")});
        QLineEdit pid(editor ? QString::number(editor->reportedPid) : QString{}, &dialog);
        QSpinBox port(&dialog); port.setRange(0, 65535); port.setSpecialValueText(QStringLiteral("Enter known port"));
        QLineEdit assembly(&dialog), pdb(&dialog);
        QLineEdit dotnet(QString::fromStdString(m_services.settings.snapshot()->get(settings::key::toolsManagedDotnetPath)), &dialog);
        form->addRow(QStringLiteral("Host"), &host); form->addRow(QStringLiteral("Backend"), &backend);
        form->addRow(QStringLiteral("Actual local process PID"), &pid); form->addRow(QStringLiteral("Known loopback debugger port"), &port);
        form->addRow(QStringLiteral("Loaded managed module (absolute)"), &assembly); form->addRow(QStringLiteral("Matching portable PDB (absolute)"), &pdb);
        form->addRow(QStringLiteral("Installed .NET source-verifier host"), &dotnet);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(&buttons);
        if (dialog.exec() != QDialog::Accepted || !owner) return;
        bool validPid = false; const auto number = pid.text().toUInt(&validPid);
        if (!validPid || !number) return;
        selected = UnityMonoDebugSelection{host.currentIndex() == 1, backend.currentIndex() == 1, number,
            static_cast<std::uint16_t>(port.value()), assembly.text().toUtf8().toStdString(), pdb.text().toUtf8().toStdString(), dotnet.text().toUtf8().toStdString()};
    }
    if (!selected) return;
    if (selected->il2cpp) { notify(QStringLiteral("Unity Debugger"), QStringLiteral("IL2CPP has no verified provider in this profile. CoreCLR and Mono wire compatibility are not assumed."), ui::NotificationKind::Error); return; }
    const auto selection = *selected;
    const auto liveTrust = m_services.trust.snapshot(root);
    if (m_projectRoot != root || !liveTrust.trusted || liveTrust.revision != authorized.revision || !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy()) return;
    if (!selection.pid || !selection.port || selection.assembly.empty() || selection.pdb.empty()) return;
    if (!selection.player && (!editor || selection.pid != static_cast<std::uint32_t>(editor->reportedPid) || !editor->epoch || editor->epoch->load() != editor->revision)) {
        notify(QStringLiteral("Unity Debugger"), QStringLiteral("Select the matching live Unity Editor session before attaching."), ui::NotificationKind::Error); return;
    }
    const auto bundle = QCoreApplication::applicationDirPath();
    const auto held = (bundle + QStringLiteral("/frost_held_read.exe")).toStdString();
    const auto guard = m_options.managedPdbGuardPath.empty() ? (bundle + QStringLiteral("/FrostManagedDebug/PdbGuard/PdbGuard.dll")).toStdString() : m_options.managedPdbGuardPath;
    if (!QFileInfo::exists(QString::fromStdString(held)) || !QFileInfo::exists(QString::fromStdString(guard))) {
        notify(QStringLiteral("Unity Debugger"), QStringLiteral("The installed held-read/source-verification helper is unavailable."), ui::NotificationKind::Error); return;
    }
    auto job = std::make_shared<ManagedSnapshotJob>(); job->target = this;
    const auto identityCurrent = [editor, player = selection.player] {
        return player || (editor && editor->epoch && editor->epoch->load() == editor->revision);
    };
    auto* trust = &m_services.trust;
    const auto admitted = [job, trust, root, revision = authorized.revision, identityCurrent] {
        const auto state = trust->snapshot(root);
        return !job->cancelled.load() && state.trusted && state.revision == revision && identityCurrent();
    };
    auto peer = debug::mono::WindowsPeer::select(selection.pid, admitted);
    if (!peer) { notify(QStringLiteral("Unity Debugger"), QString::fromStdString(peer.error().message()), ui::NotificationKind::Error); return; }
    const auto identity = (*peer)->identity();
    QMessageBox confirmation(QMessageBox::Question, QStringLiteral("Confirm Unity Process"),
        QStringLiteral("Attach the Mono 2.58 debugger to this held process?\n\nPID: %1\nImage: %2\nCreation identity: %3\nProject: %4\nPort: %5\nModule: %6\n\nThe adapter may suspend this host. Stop disconnects; it does not kill the Unity process.")
            .arg(identity.pid).arg(QString::fromStdString(identity.image)).arg(identity.creationTime).arg(QString::fromStdString(root)).arg(selection.port).arg(QString::fromStdString(selection.assembly)),
        QMessageBox::Yes | QMessageBox::Cancel, nullptr);
    confirmation.setTextFormat(Qt::PlainText); confirmation.setDefaultButton(QMessageBox::Cancel);
    const QPointer<Workbench> owner(this); connect(this, &QObject::destroyed, &confirmation, &QDialog::reject);
    if (!m_options.selectUnityDebug && (confirmation.exec() != QMessageBox::Yes || !owner)) return;
    if (!admitted() || m_projectRoot != root || !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy()) return;
    for (auto* view : m_editors->views()) if (view->document().dirty()) {
        notify(QStringLiteral("Unity Debugger"), QStringLiteral("Save and let Unity compile dirty documents before capturing a loaded module."), ui::NotificationKind::Error); return;
    }
    m_managedDebugRoot = root; m_managedSnapshotJob = job;
    auto finish = [this, job, root, selection, peer = *peer, admitted](Result<MonoAdmittedModule> evidence) {
        if (m_managedSnapshotJob != job) return;
        if (!admitted() || m_projectRoot != root || m_debug->debugging() || m_buildService->externalLaunchAdmissionBusy()) { invalidateManagedDebug(); notify(QStringLiteral("Unity Debugger"), QStringLiteral("The authorized project or Unity session changed."), ui::NotificationKind::Error); return; }
        if (!evidence) { m_managedSnapshotJob.reset(); notify(QStringLiteral("Unity Debugger"), QString::fromStdString(evidence.error().message()), ui::NotificationKind::Error); return; }
        auto module = makeVerifiedMonoModule(selection.assembly, *evidence->snapshot, *evidence->map);
        if (!module) { m_managedSnapshotJob.reset(); notify(QStringLiteral("Unity Debugger"), QString::fromStdString(module.error().message()), ui::NotificationKind::Error); return; }
        m_managedDebugSnapshot = std::make_shared<ManagedSourceSnapshot>(evidence->snapshot->source);
        std::vector<std::string> sources;
        for (const auto& [path, bytes] : m_managedDebugSnapshot->sources) { (void)bytes; sources.push_back(path); }
        auto breakpoints = m_debug->breakpoints().sessionLines();
        std::erase_if(breakpoints, [&sources](const auto& entry) { return std::find(sources.begin(), sources.end(), entry.first) == sources.end(); });
        for (const auto& [path, lines] : breakpoints) {
            (void)path;
            for (const auto& line : lines) if (line.enabled && (!line.condition.empty() || !line.hitCondition.empty() || !line.logMessage.empty())) {
                invalidateManagedDebug(); notify(QStringLiteral("Unity Debugger"), QStringLiteral("This Mono profile does not support conditional, hit-count or log breakpoints."), ui::NotificationKind::Error); return;
            }
        }
        debug::mono::AdapterTarget adapter;
        adapter.module = *module;
        adapter.connect = [peer, port = selection.port, admitted](debug::mono::MonoDeadline deadline, std::stop_token stop) -> Result<std::unique_ptr<debug::mono::BinaryLoopback>> {
            if (!admitted()) return fail(ErrorCode::Cancelled, "Unity debug admission was revoked");
            return debug::mono::BinaryLoopback::connect(port, deadline, stop,
                [peer](std::uint16_t local, std::uint16_t remote, debug::mono::MonoDeadline until, std::stop_token token) { return peer->verify(local, remote, until, token); });
        };
        debug::DebugTarget target;
        target.name = selection.player ? "Unity Mono Player" : "Unity Editor Mono";
        target.program = peer->identity().image; target.workingDirectory = root; target.trustRoots = {root};
        auto started = m_debug->attachManagedTarget(target, debug::mono::monoAdapterFactory(std::move(adapter)),
            [this, admitted](const debug::DebugFrame& frame) { return admitted() && managedDebugFrameAllowed(frame); }, sources,
            std::move(breakpoints),
            [this, admitted](std::string path, std::function<void(bool)> done) {
                if (!admitted()) { done(false); return; }
                validateManagedBreakpoint(std::move(path), [admitted, done = std::move(done)](bool valid) { done(valid && admitted()); });
            }, admitted);
        if (!started) { invalidateManagedDebug(); notify(QStringLiteral("Unity Debugger"), QString::fromStdString(started.error().message()), ui::NotificationKind::Error); }
    };
    std::erase_if(m_managedSnapshotTasks, [](const auto& task) { return task.valid() && task.ready(); });
    m_managedSnapshotTasks.push_back(m_pool->spawn(sched::Lane::Background,
        prepareUnityModule(job, &m_services.launcher, &m_services.trust, root, authorized.revision, selection, held, guard, identityCurrent, std::move(finish))));
#endif
}
}
