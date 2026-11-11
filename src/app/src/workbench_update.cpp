// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): updates, the update check dialog, applying a verified update
// and the version switcher (split from workbench.cpp, FND-12a).

#include "frost/app/welcome_screen.hpp"
#include "frost/app/workbench.hpp"
#include "frost/core/build_info.hpp"
#include "frost/io/process.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/file_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/update/http_client.hpp"
#include "frost/update/network_policy.hpp"
#include "frost/update/safe_updater.hpp"
#include "frost/update/verification.hpp"
#include "frost/vcs/git_locator.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include "update_check.hpp"
#include "update_download.hpp"
#include "update_state.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::app {

namespace key = settings::key;

namespace {

struct AsyncInstallState {
    std::mutex mutex;
    std::optional<Result<update::UpdateOutcome>> outcome;
    bool done = false;
};

class InstallWorkerGuard final : public QObject {
public:
    explicit InstallWorkerGuard(QObject* parent) : QObject(parent) {}
    ~InstallWorkerGuard() override {
        if (worker.joinable())
            worker.join();
    }
    std::thread worker;
};

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// Starts `executable` (absolute) so that it outlives FrostIDE: not contained (killOnClose off), no
// handle of this process inherited. Qt's detached start is not used (CLAUDE.md: child processes
// only through IProcessLauncher). Errors: the launcher's.
Result<void> launchDetached(io::IProcessLauncher& launcher, const QString& executable, std::vector<std::string> args,
                            const QString& workingDirectory) {
    io::ProcessSpec spec;
    spec.executable = QDir::toNativeSeparators(executable).toStdString();
    spec.arguments = std::move(args);
    spec.workingDirectory = QDir::toNativeSeparators(workingDirectory).toStdString();
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Null;
    spec.stderrMode = io::Stdio::Null;
    spec.killOnClose = false;
    auto started = launcher.launch(spec);
    if (!started)
        return std::unexpected(std::move(started.error()));
    return {}; // Destroying the uncontained IProcess leaves the child running.
}

// Windows PowerShell 5.1 by absolute path (<system directory>\WindowsPowerShell\v1.0), never by
// bare name. Errors: Unsupported off Windows, NotFound.
Result<QString> windowsPowerShell() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return fail(ErrorCode::IoError, "GetSystemDirectoryW failed");
    QString path = QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length)) +
                   QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (!QFileInfo(path).isFile())
        return fail(ErrorCode::NotFound, "Windows PowerShell was not found at " + path.toStdString());
    return path;
#else
    return fail(ErrorCode::Unsupported, "installing updates is only implemented on Windows (FND-11)");
#endif
}

} // namespace

void Workbench::showUpdateCheck() {
    const QPointer<Workbench> guard(this);
    QWidget* parentWin = activeTopWindow();
    if (!parentWin || !interactiveReady() || m_updateChecking)
        return;
    auto snapshot = m_services.settings.snapshot();
    auto netPolicy = std::make_shared<update::NetworkPolicy>(snapshot->get(key::networkOffline));
    if (netPolicy->isOffline()) {
        QMessageBox::information(parentWin, tr("Updates"),
                                 tr("FrostIDE is working offline. Change this in Settings > Updates."));
        return;
    }
    QByteArray configuredKey(FROST_UPDATE_PUBLIC_KEY_HEX);
    if (configuredKey.isEmpty()) {
        // Default Ed25519 release verification public key when not overridden via CMake.
        configuredKey = QByteArrayLiteral("c90263efb760658afedf515b2889f50cd8de7261009e7ae9da5e8038126dde1f");
    }
    const QByteArray decoded = QByteArray::fromHex(configuredKey);
    if (configuredKey.size() != 64 || decoded.size() != 32) {
        QMessageBox::information(parentWin, tr("Updates unavailable"),
                                 tr("This build does not include an official update verification key."));
        return;
    }
    netPolicy->setConsentPrompt([guard](const update::ConsentRequest&) -> update::ConsentDecision {
        if (!guard || !guard->interactiveReady() || guard->m_services.settings.snapshot()->get(key::networkOffline))
            return update::ConsentDecision::Denied;
        QWidget* win = guard->activeTopWindow();
        if (!win)
            return update::ConsentDecision::Denied;
        const auto consent = QMessageBox::question(
            win, Workbench::tr("Check for updates"),
            Workbench::tr(
                "Connect to GitHub to check Frowsty/FrostIDE releases? GitHub will receive your network address."));
        return (consent == QMessageBox::Yes) ? update::ConsentDecision::Granted : update::ConsentDecision::Denied;
    });
    const auto authorized = netPolicy->checkAndRequestConsent(
        "https://api.github.com/repos/Frowsty/FrostIDE/releases?per_page=100", "check_github_releases");
    if (!authorized || !guard || !interactiveReady() || m_services.settings.snapshot()->get(key::networkOffline))
        return;
    frost::update::PublicKey publicKey{};
    std::memcpy(publicKey.data(), decoded.constData(), publicKey.size());
    bool buildOk = false;
    const auto installedBuild = QByteArray(FROST_UPDATE_BUILD_NUMBER).toULongLong(&buildOk);
    if (!buildOk) {
        QMessageBox::warning(parentWin, tr("Updates unavailable"),
                             tr("This build has an invalid update build number."));
        return;
    }
    const auto localData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (localData.isEmpty())
        return;
    const auto stateDirectory = localData + QStringLiteral("/updates");
    auto floor = readUpdateFloor(stateDirectory);
    if (!floor) {
        QMessageBox::warning(parentWin, tr("Updates unavailable"), QString::fromStdString(floor.error().message()));
        return;
    }
#ifdef _WIN32
    constexpr std::string_view platform = "windows";
#else
    constexpr std::string_view platform = "linux";
#endif
    const auto channel = m_services.settings.snapshot()->get(key::updateChannel);
    update::ReleasePolicy policy{channel,
                                 platform,
                                 "x64",
                                 static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()),
                                 installedBuild,
                                 floor->build,
                                 build_info::kVersion,
                                 floor->version,
                                 floor->digest};
    auto revalidate = [stateDirectory, publicKey, installedBuild, channel,
                       platform](const update::AuthenticatedRelease& envelope) {
        update::ReleasePolicy fresh{channel,
                                    platform,
                                    "x64",
                                    static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()),
                                    installedBuild,
                                    0,
                                    build_info::kVersion,
                                    "",
                                    ""};
        return acceptUpdate(stateDirectory, envelope, publicKey, fresh);
    };
    auto cancelled = std::make_shared<bool>(false);
    auto ready = [self = guard, channel, cancelled, netPolicy] {
        const bool offline = !self || self->m_services.settings.snapshot()->get(key::networkOffline);
        if (offline)
            netPolicy->setOffline(true);
        return !*cancelled && self && self->interactiveReady() && !offline &&
               self->m_services.settings.snapshot()->get(key::updateChannel) == channel;
    };
    m_updateChecking = true;
    const QPointer<QProgressDialog> progress =
        new QProgressDialog(tr("Checking signed GitHub releases…"), tr("Cancel"), 0, 0, parentWin);
    progress->setWindowTitle(tr("Check for updates"));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    connect(progress, &QProgressDialog::canceled, progress, [cancelled] { *cancelled = true; });
    progress->show();
    if (!guard)
        return;
    auto httpClient = std::make_shared<update::PolicyGatedHttpClient>(update::createNativeHttpClient(), netPolicy);
    auto* check = new UpdateCheck(
        httpClient, publicKey, policy, ready,
        [] { return static_cast<std::uint64_t>(QDateTime::currentSecsSinceEpoch()); },
        [self = guard, httpClient, ready, revalidate, progress, cancelled,
         channel](Result<update::AuthenticatedRelease> result) {
            if (progress)
                progress->deleteLater();
            if (!self)
                return;
            self->m_updateChecking = false;
            QWidget* activeWin = self->activeTopWindow();
            if (!activeWin)
                return;
            if (*cancelled)
                return;
            if (!result) {
                if (result.error().code() == ErrorCode::NotFound) {
                    const QString statusMsg = tr("FrostIDE %1 • Channel: %2 (Up to date — checked just now)")
                                                  .arg(qs(build_info::kVersion), QString::fromStdString(channel));
                    if (self->m_welcome)
                        self->m_welcome->setUpdateStatus(statusMsg);
                    if (self->m_launcherWindow && self->m_launcherWindow->welcomeScreen())
                        self->m_launcherWindow->welcomeScreen()->setUpdateStatus(statusMsg);
                    QMessageBox::information(activeWin, tr("Check for Updates"),
                                             tr("You already have the latest version of FrostIDE (%1, channel: %2).")
                                                 .arg(qs(build_info::kVersion), QString::fromStdString(channel)));
                } else {
                    QMessageBox::information(activeWin, tr("Update check"),
                                             QString::fromUtf8(result.error().message().c_str()));
                }
                return;
            }
            if (!ready())
                return;
            auto accepted = revalidate(*result);
            if (!accepted) {
                QMessageBox::warning(activeWin, tr("Update check"), QString::fromStdString(accepted.error().message()));
                return;
            }
            result->release = std::move(*accepted);
            const auto answer =
                QMessageBox::question(activeWin, tr("Update available"),
                                      tr("A signed FrostIDE update is available: %1 (build %2). Download it now?")
                                          .arg(QString::fromStdString(result->release.version))
                                          .arg(result->release.build));
            if (answer != QMessageBox::Yes || !ready())
                return;
            // A different instance may have advanced the floor during the dialog.
            accepted = revalidate(*result);
            if (!accepted) {
                QMessageBox::warning(activeWin, tr("Update download"),
                                     QString::fromStdString(accepted.error().message()));
                return;
            }
            result->release = std::move(*accepted);
            self->m_updateChecking = true;
            auto envelope = std::make_shared<update::AuthenticatedRelease>(std::move(*result));
            auto downloadCancelled = std::make_shared<bool>(false);
            auto downloadReady = [ready, downloadCancelled] { return !*downloadCancelled && ready(); };
            const QPointer<QProgressDialog> downloadProgress =
                new QProgressDialog(tr("Downloading and verifying update…"), tr("Cancel"), 0, 0, activeWin);
            downloadProgress->setWindowTitle(tr("Download update"));
            downloadProgress->setMinimumDuration(0);
            downloadProgress->setAutoClose(false);
            connect(downloadProgress, &QProgressDialog::canceled, downloadProgress,
                    [downloadCancelled] { *downloadCancelled = true; });
            downloadProgress->show();
            if (!self)
                return;
            auto* download = new UpdateDownload(
                httpClient, envelope->release, downloadReady,
                [self, envelope, downloadReady, revalidate, downloadProgress,
                 downloadCancelled](Result<std::unique_ptr<QTemporaryFile>> payload) {
                    if (downloadProgress)
                        downloadProgress->deleteLater();
                    if (!self)
                        return;
                    self->m_updateChecking = false;
                    QWidget* dlWin = self->activeTopWindow();
                    if (!dlWin)
                        return;
                    if (*downloadCancelled)
                        return;
                    if (!payload) {
                        QMessageBox::warning(dlWin, tr("Update download"),
                                             QString::fromUtf8(payload.error().message().c_str()));
                        return;
                    }
                    if (!downloadReady())
                        return;
                    auto current = revalidate(*envelope);
                    if (!current) {
                        QMessageBox::warning(dlWin, tr("Update download"),
                                             QString::fromStdString(current.error().message()));
                        return;
                    }
                    envelope->release = std::move(*current);
                    self->m_verifiedUpdate = std::move(*payload);
                    self->m_verifiedRelease = std::move(*envelope);
                    const QString readyMsg = tr("Update %1 (build %2) verified & ready to install")
                                                 .arg(QString::fromStdString(self->m_verifiedRelease->release.version))
                                                 .arg(self->m_verifiedRelease->release.build);
                    if (self->m_welcome)
                        self->m_welcome->setUpdateStatus(readyMsg);
                    if (self->m_launcherWindow && self->m_launcherWindow->welcomeScreen())
                        self->m_launcherWindow->welcomeScreen()->setUpdateStatus(readyMsg);
                    self->applyVerifiedUpdate();
                },
                self->m_window);
            download->start();
        },
        m_window);
    check->start();
}

void Workbench::applyVerifiedUpdate() {
    QWidget* parentWin = activeTopWindow();
    if (!parentWin || !m_verifiedUpdate || !m_verifiedRelease)
        return;

    const QString versionStr = QString::fromStdString(m_verifiedRelease->release.version);
    const std::uint64_t buildNum = m_verifiedRelease->release.build;

    // Side-by-side builds go next to the running one; no developer machine path is assumed.
    QDir appParent(QCoreApplication::applicationDirPath());
    appParent.cdUp();
    const QString defaultBaseDir = appParent.absolutePath();
    const QString folderVersion =
        versionStr.contains(QLatin1Char('-')) ? versionStr : (versionStr + QStringLiteral("-beta"));
    const QString defaultSideBySideDir =
        QDir::toNativeSeparators(defaultBaseDir + QStringLiteral("/FrostIDE-%1").arg(folderVersion));

    QDialog dialog(parentWin);
    dialog.setObjectName(QStringLiteral("updateInstallerDialog"));
    dialog.setWindowTitle(tr("Install Verified FrostIDE Update"));
    dialog.resize(620, 290);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    auto* titleLabel = new QLabel(tr("<b>FrostIDE %1 (Build %2)</b> — Ed25519 signature and SHA-256 payload verified.")
                                      .arg(versionStr)
                                      .arg(buildNum),
                                  &dialog);
    titleLabel->setWordWrap(true);
    root->addWidget(titleLabel);

    auto* descLabel = new QLabel(
        tr("Choose how to install this update. Installing to a new side-by-side build folder preserves your previous "
           "beta builds so you can switch between versions at any time."),
        &dialog);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QStringLiteral("color: #9DA5B4;"));
    root->addWidget(descLabel);

    auto* dirForm = new QFormLayout();
    auto* dirRow = new QWidget(&dialog);
    auto* dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    auto* destEdit = new QLineEdit(defaultSideBySideDir, dirRow);
    destEdit->setObjectName(QStringLiteral("updateDestDirEdit"));
    auto* browseBtn = new QPushButton(tr("Browse..."), dirRow);
    QObject::connect(browseBtn, &QPushButton::clicked, &dialog, [&dialog, destEdit] {
        const QString chosen =
            QFileDialog::getExistingDirectory(&dialog, tr("Select Target Folder for New Build"), destEdit->text());
        if (!chosen.isEmpty())
            destEdit->setText(QDir::toNativeSeparators(chosen));
    });
    dirLayout->addWidget(destEdit, 1);
    dirLayout->addWidget(browseBtn);
    dirForm->addRow(tr("Side-by-Side Build Folder:"), dirRow);
    root->addLayout(dirForm);

    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* sideBySideBtn = new QPushButton(tr("Install to Build Folder && Restart"), &dialog);
    sideBySideBtn->setObjectName(QStringLiteral("installSideBySideButton"));
    sideBySideBtn->setDefault(true);
    auto* inPlaceBtn = new QPushButton(tr("Update Current Folder In-Place && Restart"), &dialog);
    inPlaceBtn->setObjectName(QStringLiteral("installInPlaceButton"));
    auto* laterBtn = new QPushButton(tr("Later"), &dialog);
    btnRow->addWidget(sideBySideBtn);
    btnRow->addWidget(inPlaceBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(laterBtn);
    root->addLayout(btnRow);

    int installMode = 0; // 1 = side-by-side, 2 = in-place
    QObject::connect(sideBySideBtn, &QPushButton::clicked, &dialog, [&dialog, &installMode] {
        installMode = 1;
        dialog.accept();
    });
    QObject::connect(inPlaceBtn, &QPushButton::clicked, &dialog, [&dialog, &installMode] {
        installMode = 2;
        dialog.accept();
    });
    QObject::connect(laterBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    ui::installDialogChrome(&dialog, false);
    if (dialog.exec() != QDialog::Accepted || installMode == 0)
        return;

    const QString localData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (localData.isEmpty())
        return;
    const QString updatesDir = localData + QStringLiteral("/updates");
    QDir().mkpath(updatesDir);
    const QString stagedZip =
        QDir::toNativeSeparators(updatesDir + QStringLiteral("/staged-build-%1.zip").arg(buildNum));
    QFile::remove(stagedZip);
    if (!QFile::copy(m_verifiedUpdate->fileName(), stagedZip)) {
        QMessageBox::warning(parentWin, tr("Update Installation Failed"),
                             tr("Could not stage update archive to %1.").arg(stagedZip));
        return;
    }

    if (installMode == 1) {
        const QString targetDir = QDir::fromNativeSeparators(destEdit->text().trimmed());
        if (targetDir.isEmpty())
            return;

        const QString backupDir = QDir::toNativeSeparators(updatesDir + QStringLiteral("/rollback-%1").arg(buildNum));
        update::SafeUpdateSpec spec;
        spec.installDir = QDir::toNativeSeparators(targetDir).toStdString();
        spec.backupDir = backupDir.toStdString();
        spec.payloadPath = stagedZip.toStdString();
        spec.expectedRelease = m_verifiedRelease->release;
        spec.executableRelativePath = std::string(update::kDefaultExecutableName);
        spec.healthCheckArgs = {"--selftest"};
        spec.healthCheckTimeout = std::chrono::milliseconds{30000};
        spec.launchDetachedAfterInstall = true;

        const QPointer<QProgressDialog> installProgress = new QProgressDialog(
            tr("Installing and verifying FrostIDE %1 in %2…").arg(versionStr, QDir::toNativeSeparators(targetDir)),
            QString(), 0, 0, parentWin);
        installProgress->setWindowTitle(tr("Installing Update"));
        installProgress->setMinimumDuration(0);
        installProgress->setCancelButton(nullptr);
        installProgress->setAutoClose(false);
        installProgress->show();
        m_updateChecking = true;

        io::IFileSystem* fs = &m_services.fs;
        io::IProcessLauncher* launcher = &m_services.launcher;
        auto state = std::make_shared<AsyncInstallState>();
        auto* workerGuard = new InstallWorkerGuard(this);
        workerGuard->worker = std::thread([state, fs, launcher, spec = std::move(spec)]() {
            update::SafeUpdater updater(*fs, *launcher);
            auto outcome = updater.applyUpdate(spec);
            std::lock_guard lock(state->mutex);
            state->outcome = std::move(outcome);
            state->done = true;
        });

        auto* pollTimer = new QTimer(workerGuard);
        pollTimer->setInterval(20);
        connect(pollTimer, &QTimer::timeout, this,
                [this, workerGuard, pollTimer, installProgress, state, versionStr, targetDir]() {
                    std::optional<Result<update::UpdateOutcome>> outcome;
                    {
                        std::lock_guard lock(state->mutex);
                        if (!state->done)
                            return;
                        outcome = std::move(state->outcome);
                    }
                    pollTimer->stop();
                    workerGuard->deleteLater();
                    if (installProgress)
                        installProgress->deleteLater();
                    m_updateChecking = false;
                    QWidget* activeWin = activeTopWindow();
                    if (!outcome || !*outcome) {
                        if (activeWin && outcome) {
                            QMessageBox::warning(activeWin, Workbench::tr("Update Installation Failed"),
                                                 qs(outcome->error().message()));
                        }
                        return;
                    }
                    if ((*outcome)->rolledBack) {
                        if (activeWin) {
                            QMessageBox::warning(
                                activeWin, Workbench::tr("Update Rolled Back"),
                                Workbench::tr("FrostIDE %1 failed its post-install health check (%2) and was "
                                              "automatically rolled back.%3")
                                    .arg(versionStr, qs((*outcome)->failureReason),
                                         (*outcome)->previousVersionStarted
                                             ? Workbench::tr(" The previous version passed verification.")
                                             : QString()));
                        }
                        return;
                    }
                    if ((*outcome)->applied) {
                        m_exitConfirmed = true; // The restart is the user's own choice in this dialog.
                        // A vetoed close (the unsaved-changes prompt) leaves later closes to general.confirmExit.
                        if (m_window && !m_window->close())
                            m_exitConfirmed = false;
                        QCoreApplication::quit();
                    } else if (activeWin) {
                        QMessageBox::warning(activeWin, Workbench::tr("Update Installed"),
                                             Workbench::tr("Update installed to %1, but could not be started.")
                                                 .arg(QDir::toNativeSeparators(targetDir)));
                    }
                });
        pollTimer->start();
        return;
    }

    if (installMode == 2) {
        update::SafeUpdater verifier(m_services.fs, m_services.launcher);
        if (auto verified = verifier.verifyPayloadFile(stagedZip.toStdString(), m_verifiedRelease->release);
            !verified) {
            QMessageBox::warning(parentWin, tr("Update Installation Failed"), qs(verified.error().message()));
            return;
        }
        const QString scriptPath = QDir::toNativeSeparators(updatesDir + QStringLiteral("/apply-update.ps1"));
        const QString backupDir = QDir::toNativeSeparators(updatesDir + QStringLiteral("/rollback-inplace"));
        const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
        const QString appExe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const qint64 pid = QCoreApplication::applicationPid();

        QFile scriptFile(scriptPath);
        if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QMessageBox::warning(parentWin, tr("Update Installation Failed"),
                                 tr("Could not write updater script to %1.").arg(scriptPath));
            return;
        }
        QString psStagedZip = stagedZip;
        psStagedZip.replace(QLatin1Char('\''), QStringLiteral("''"));
        QString psAppDir = appDir;
        psAppDir.replace(QLatin1Char('\''), QStringLiteral("''"));
        QString psAppExe = appExe;
        psAppExe.replace(QLatin1Char('\''), QStringLiteral("''"));
        QString psBackupDir = backupDir;
        psBackupDir.replace(QLatin1Char('\''), QStringLiteral("''"));

        const QString scriptContent =
            QStringLiteral(
                "$ErrorActionPreference = 'SilentlyContinue'\n"
                "$appDir = '%3'\n"
                "$backupDir = '%5'\n"
                "Wait-Process -Id %1 -Timeout 30\n"
                "Get-Process crashpad_handler, CSharpLanguageServer, frost_held_read -ErrorAction SilentlyContinue | "
                "Where-Object {\n"
                "    $_.Path -and $_.Path.StartsWith($appDir, [System.StringComparison]::OrdinalIgnoreCase)\n"
                "} | Stop-Process -Force -ErrorAction SilentlyContinue\n"
                "Start-Sleep -Milliseconds 600\n"
                "if (Test-Path -LiteralPath $backupDir) { Remove-Item -LiteralPath $backupDir -Recurse -Force }\n"
                "New-Item -ItemType Directory -Path $backupDir -Force | Out-Null\n"
                "Get-ChildItem -LiteralPath $appDir -Force | ForEach-Object {\n"
                "    Copy-Item -LiteralPath $_.FullName -Destination $backupDir -Recurse -Force\n"
                "}\n"
                "$tmp = Join-Path $env:TEMP ('frost-update-' + [guid]::NewGuid().ToString('N'))\n"
                "New-Item -ItemType Directory -Path $tmp -Force | Out-Null\n"
                "Expand-Archive -LiteralPath '%2' -DestinationPath $tmp -Force\n"
                "$src = $tmp\n"
                "if (-not (Test-Path -LiteralPath (Join-Path $src 'frost.exe'))) {\n"
                "    $sub = Get-ChildItem -LiteralPath $tmp -Directory | Select-Object -First 1\n"
                "    if ($sub -and (Test-Path -LiteralPath (Join-Path $sub.FullName 'frost.exe'))) { $src = "
                "$sub.FullName }\n"
                "}\n"
                "Get-ChildItem -LiteralPath $src -Force | ForEach-Object {\n"
                "    Copy-Item -LiteralPath $_.FullName -Destination $appDir -Recurse -Force\n"
                "}\n"
                "Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue\n"
                "$env:QT_QPA_PLATFORM = 'offscreen'\n"
                "$check = Start-Process -FilePath '%4' -ArgumentList '--selftest' -WorkingDirectory $appDir -Wait "
                "-PassThru -WindowStyle Hidden\n"
                "Remove-Item Env:\\QT_QPA_PLATFORM -ErrorAction SilentlyContinue\n"
                "if ($null -eq $check -or $check.ExitCode -ne 0) {\n"
                "    Get-ChildItem -LiteralPath $appDir -Force | Remove-Item -Recurse -Force -ErrorAction "
                "SilentlyContinue\n"
                "    Get-ChildItem -LiteralPath $backupDir -Force | ForEach-Object {\n"
                "        Copy-Item -LiteralPath $_.FullName -Destination $appDir -Recurse -Force\n"
                "    }\n"
                "}\n"
                "Start-Process -FilePath '%4' -WorkingDirectory $appDir\n")
                .arg(pid)
                .arg(psStagedZip, psAppDir, psAppExe, psBackupDir);
        scriptFile.write(scriptContent.toUtf8());
        scriptFile.close();

        auto powershell = windowsPowerShell();
        if (!powershell) {
            QMessageBox::warning(parentWin, tr("Update Installation Failed"), qs(powershell.error().message()));
            return;
        }
        // The script waits for this process to exit, so it must not be contained (launchDetached).
        if (auto started = launchDetached(m_services.launcher, *powershell,
                                          {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                           "-WindowStyle", "Hidden", "-File", scriptPath.toStdString()},
                                          updatesDir);
            started) {
            m_exitConfirmed = true; // The restart is the user's own choice in this dialog.
            // A vetoed close (the unsaved-changes prompt) leaves later closes to general.confirmExit.
            if (m_window && !m_window->close())
                m_exitConfirmed = false;
            QCoreApplication::quit();
        } else {
            QMessageBox::warning(parentWin, tr("Update Installation Failed"),
                                 tr("Could not start the updater script: %1").arg(qs(started.error().message())));
        }
    }
}

void Workbench::showVersionSwitcher() {
    QWidget* parentWin = activeTopWindow();
    if (!parentWin)
        return;

    QDialog dialog(parentWin);
    dialog.setObjectName(QStringLiteral("versionSwitcherDialog"));
    dialog.setWindowTitle(tr("Switch FrostIDE Version / Manage Installations"));
    dialog.resize(660, 400);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* headerLabel =
        new QLabel(tr("<b>Installed FrostIDE Builds</b> — Running: <b>%1</b> (<code>%2</code>)")
                       .arg(qs(build_info::kVersion), QDir::toNativeSeparators(QCoreApplication::applicationDirPath())),
                   &dialog);
    headerLabel->setWordWrap(true);
    root->addWidget(headerLabel);

    auto* listWidget = new QListWidget(&dialog);
    listWidget->setObjectName(QStringLiteral("installedVersionsList"));

    const QString currentAppDir = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    // Side-by-side builds live next to the running one (applyVerifiedUpdate's default folder).
    QStringList scanRoots;
    {
        QDir parentDir(QCoreApplication::applicationDirPath());
        if (parentDir.cdUp()) {
            const QString parentPath = parentDir.absolutePath();
            if (!scanRoots.contains(parentPath, Qt::CaseInsensitive))
                scanRoots.append(parentPath);
        }
    }

    struct InstalledBuildEntry {
        QString name;
        QString exePath;
        QString dirPath;
        QDateTime lastModified;
        bool isCurrent = false;
    };
    std::vector<InstalledBuildEntry> builds;
    QStringList seenCanonicalDirs;

    auto addBuildIfValid = [&](const QString& dirPath) {
        const QFileInfo exeInfo(QDir(dirPath).filePath(QStringLiteral("frost.exe")));
        if (!exeInfo.exists() || !exeInfo.isFile())
            return;
        const QString canonicalDir = QFileInfo(dirPath).canonicalFilePath();
        if (canonicalDir.isEmpty() || seenCanonicalDirs.contains(canonicalDir, Qt::CaseInsensitive))
            return;
        seenCanonicalDirs.append(canonicalDir);
        InstalledBuildEntry entry;
        entry.name = QFileInfo(dirPath).fileName();
        entry.exePath = exeInfo.absoluteFilePath();
        entry.dirPath = QDir::toNativeSeparators(QFileInfo(dirPath).absoluteFilePath());
        entry.lastModified = exeInfo.lastModified();
        entry.isCurrent = (canonicalDir.compare(currentAppDir, Qt::CaseInsensitive) == 0);
        builds.push_back(std::move(entry));
    };

    addBuildIfValid(QCoreApplication::applicationDirPath());
    for (const QString& scanRoot : scanRoots) {
        QDir rootDir(scanRoot);
        if (!rootDir.exists())
            continue;
        const QFileInfoList subDirs =
            rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const QFileInfo& sub : subDirs) {
            addBuildIfValid(sub.absoluteFilePath());
        }
    }

    std::ranges::sort(builds, [](const InstalledBuildEntry& a, const InstalledBuildEntry& b) {
        if (a.isCurrent != b.isCurrent)
            return a.isCurrent;
        return a.lastModified > b.lastModified;
    });

    for (const InstalledBuildEntry& b : builds) {
        const QString label =
            b.isCurrent ? tr("%1  [Current Running Build]\n%2  •  Built %3")
                              .arg(b.name, b.dirPath, b.lastModified.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                        : tr("%1\n%2  •  Built %3")
                              .arg(b.name, b.dirPath, b.lastModified.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        auto* item = new QListWidgetItem(label, listWidget);
        item->setData(Qt::UserRole, b.exePath);
        item->setData(Qt::UserRole + 1, b.dirPath);
        item->setData(Qt::UserRole + 2, b.isCurrent);
    }
    if (listWidget->count() > 0)
        listWidget->setCurrentRow(0);
    root->addWidget(listWidget, 1);

    auto* btnRow = new QHBoxLayout();
    auto* switchBtn = new QPushButton(tr("Switch to Selected && Restart"), &dialog);
    switchBtn->setObjectName(QStringLiteral("switchAndRestartButton"));
    switchBtn->setDefault(true);
    auto* sideBySideBtn = new QPushButton(tr("Launch Side-by-Side"), &dialog);
    sideBySideBtn->setObjectName(QStringLiteral("launchSideBySideButton"));
    auto* openFolderBtn = new QPushButton(tr("Reveal in Explorer"), &dialog);
    auto* updateBtn =
        new QPushButton(m_verifiedUpdate ? tr("Install Downloaded Update...") : tr("Check for Updates..."), &dialog);
    auto* closeBtn = new QPushButton(tr("Close"), &dialog);

    btnRow->addWidget(switchBtn);
    btnRow->addWidget(sideBySideBtn);
    btnRow->addWidget(openFolderBtn);
    btnRow->addWidget(updateBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    QObject::connect(switchBtn, &QPushButton::clicked, &dialog, [&dialog, listWidget, this] {
        QListWidgetItem* cur = listWidget->currentItem();
        if (!cur)
            return;
        const QString exePath = cur->data(Qt::UserRole).toString();
        if (exePath.isEmpty() || !QFileInfo::exists(exePath))
            return;
        if (auto started = launchDetached(m_services.launcher, exePath, {}, QFileInfo(exePath).absolutePath());
            started) {
            dialog.accept();
            m_exitConfirmed = true; // The restart is the user's own choice in this dialog.
            // A vetoed close (the unsaved-changes prompt) leaves later closes to general.confirmExit.
            if (m_window && !m_window->close())
                m_exitConfirmed = false;
            QCoreApplication::quit();
        } else {
            QMessageBox::warning(&dialog, tr("Switch FrostIDE Version"), qs(started.error().message()));
        }
    });
    QObject::connect(sideBySideBtn, &QPushButton::clicked, &dialog, [&dialog, listWidget, this] {
        QListWidgetItem* cur = listWidget->currentItem();
        if (!cur)
            return;
        const QString exePath = cur->data(Qt::UserRole).toString();
        if (!exePath.isEmpty() && QFileInfo::exists(exePath)) {
            if (auto started = launchDetached(m_services.launcher, exePath, {}, QFileInfo(exePath).absolutePath());
                !started)
                QMessageBox::warning(&dialog, tr("Launch Side-by-Side"), qs(started.error().message()));
        }
    });
    QObject::connect(openFolderBtn, &QPushButton::clicked, &dialog, [listWidget, this] {
        QListWidgetItem* cur = listWidget->currentItem();
        if (!cur)
            return;
        const QString dirPath = cur->data(Qt::UserRole + 1).toString();
        if (!dirPath.isEmpty()) {
            (void)ui::revealInFileManager(m_services.launcher, dirPath.toStdString(), true);
        }
    });
    QObject::connect(updateBtn, &QPushButton::clicked, &dialog, [&dialog, this] {
        dialog.accept();
        if (m_verifiedUpdate && m_verifiedRelease)
            applyVerifiedUpdate();
        else
            showUpdateCheck();
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    ui::installDialogChrome(&dialog, false);
    dialog.exec();
}

} // namespace frost::app
