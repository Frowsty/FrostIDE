// SPDX-License-Identifier: Apache-2.0
// Explicit local native IL2CPP player launch; all debugger results come from the existing LLDB DAP.
#include "frost/app/workbench.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include "unity_native_profile.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
namespace frost::app {
void Workbench::startUnityIl2cppNativeDebug() {
    if (!m_window || !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy())
        return;
    const auto root = m_projectRoot;
    const auto authorization = m_services.trust.snapshot(root);
    if (!authorization.trusted) {
        notify(QStringLiteral("IL2CPP native debugger"), QStringLiteral("Trust the Unity project first."),
               ui::NotificationKind::Error);
        return;
    }
    std::optional<UnityIl2cppNativeSelection> selected;
    if (m_options.selectUnityNativeDebug)
        selected = m_options.selectUnityNativeDebug(root);
    else {
        QDialog dialog;
        const QPointer<Workbench> owner(this);
        connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
        dialog.setWindowTitle(QStringLiteral("Debug IL2CPP Generated C++"));
        auto* form = new QFormLayout(&dialog);
        auto* description = new QLabel(
            QStringLiteral("Launch a local player with the native LLDB debugger. Provide the player, native module, "
                           "native symbols and generated C++ from the same IL2CPP build. Native adapter results "
                           "determine symbol availability and breakpoint binding. This profile provides generated C++ "
                           "debugging; C# script sequence points and managed Unity values remain unavailable."),
            &dialog);
        description->setWordWrap(true);
        form->addRow(description);
        QLineEdit player(&dialog), module(&dialog), symbols(&dialog), sources(&dialog), from(&dialog);
        form->addRow(QStringLiteral("Player executable (absolute)"), &player);
        form->addRow(QStringLiteral("IL2CPP native module (absolute)"), &module);
        form->addRow(QStringLiteral("Matching native PDB/DWARF file (absolute)"), &symbols);
        form->addRow(QStringLiteral("Generated C++ directory (absolute)"), &sources);
        form->addRow(QStringLiteral("Source prefix recorded at build time (optional)"), &from);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(&buttons);
        if (dialog.exec() != QDialog::Accepted || !owner)
            return;
        selected =
            UnityIl2cppNativeSelection{player.text().toUtf8().toStdString(), module.text().toUtf8().toStdString(),
                                       symbols.text().toUtf8().toStdString(), sources.text().toUtf8().toStdString(),
                                       from.text().toUtf8().toStdString()};
    }
    if (!selected)
        return;
    const auto current = m_services.trust.snapshot(root);
    if (m_projectRoot != root || !current.trusted || current.revision != authorization.revision ||
        !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy())
        return;
    auto profile = prepareUnityNativeProfile(m_services.fs, m_services.trust, root, *selected);
    if (!profile) {
        notify(QStringLiteral("IL2CPP native debugger"), QString::fromStdString(profile.error().message()),
               ui::NotificationKind::Error);
        return;
    }
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("Launch IL2CPP Native Player"),
                        QStringLiteral("Launch this local player under LLDB?\n\nPlayer: %1\nNative module: %2\nNative "
                                       "symbols: %3\nGenerated C++: %4\n\nStop ends the launched player. Missing or "
                                       "mismatched symbols may leave native breakpoints unverified.")
                            .arg(QString::fromStdString(selected->player), QString::fromStdString(selected->module),
                                 QString::fromStdString(selected->symbols),
                                 QString::fromStdString(selected->generatedRoot)),
                        QMessageBox::Yes | QMessageBox::Cancel, nullptr);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::Cancel);
    const QPointer<Workbench> owner(this);
    connect(this, &QObject::destroyed, &confirm, &QDialog::reject);
    if (!m_options.selectUnityNativeDebug && (confirm.exec() != QMessageBox::Yes || !owner))
        return;
    if (m_projectRoot != root || !canStartLuaDebug() || m_buildService->externalLaunchAdmissionBusy())
        return;
    auto* fs = &m_services.fs;
    auto* trust = &m_services.trust;
    const auto admission = [fs, trust, root, revision = authorization.revision,
                            selection = *selected]() -> Result<void> {
        const auto state = trust->snapshot(root);
        if (!state.trusted || state.revision != revision)
            return fail(ErrorCode::Untrusted, "IL2CPP native launch authorization changed");
        auto checked = prepareUnityNativeProfile(*fs, *trust, root, selection);
        if (!checked)
            return std::unexpected(std::move(checked.error()));
        return {};
    };
    debug::AdapterFactory factory = m_options.debugAdapterFactory;
    if (!factory) {
        auto path = debug::findLldbDap(m_services.fs, debug::defaultLldbDapSearch(m_services.settings.snapshot()->get(
                                                          settings::key::toolsLldbDapPath)));
        if (!path) {
            notify(QStringLiteral("IL2CPP native debugger"), QString::fromStdString(path.error().message()),
                   ui::NotificationKind::Error);
            return;
        }
        factory = debug::lldbDapAdapterFactory(m_services.launcher, *path);
    }
    const auto started = m_debug->startAdapterTarget(
        profile->target, std::move(factory), std::move(profile->arguments), "lldb-dap", debug::DebugTargetMode::Launch,
        false, admission, [this] { return canStartLuaDebug() && !m_buildService->externalLaunchAdmissionBusy(); },
        [fs, generatedRoot = profile->generatedRoot](std::string_view path) {
            return isUnityGeneratedSource(*fs, generatedRoot, path);
        });
    if (!started)
        notify(QStringLiteral("IL2CPP native debugger"), QString::fromStdString(started.error().message()),
               ui::NotificationKind::Error);
}
} // namespace frost::app
