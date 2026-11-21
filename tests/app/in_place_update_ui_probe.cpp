// SPDX-License-Identifier: Apache-2.0
// Real Workbench dialogs with an isolated, locally authenticated update offer.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <frost/app/main_window.hpp>
#include <frost/app/workbench.hpp>
#include <frost/core/clock.hpp>
#include <frost/io/async_process.hpp>
#include <frost/io/file_system.hpp>
#include <frost/project/trust_store.hpp>
#include <frost/settings/keys.hpp>
#include <frost/settings/registry.hpp>
#include <frost/settings/store.hpp>
#include <frost/ui/shell/editor_tabs.hpp>
#include <frost/ui/shell/notifications.hpp>
#include <frost/ui/workbench/workbench.hpp>
#include <frost/update/install_handoff.hpp>
#include <frost/update/manifest.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <span>

#include <sodium.h>

namespace frost::app {
struct WorkbenchUpdateFixture {
    static void offer(Workbench& workbench, std::unique_ptr<QTemporaryFile> payload,
                      update::AuthenticatedRelease release) {
        workbench.m_verifiedUpdate = std::move(payload);
        workbench.m_verifiedRelease = std::move(release);
        workbench.applyVerifiedUpdate();
    }
};
} // namespace frost::app

namespace {
using namespace frost;
using namespace frost::app;

class ObservingLauncher final : public io::IProcessLauncher {
public:
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (spec.executable.ends_with(update::kPerUserUpdateHelperName))
            ++updaters;
        return io::realProcessLauncher().launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return io::realProcessLauncher().launchAsync(spec, context);
    }
    int updaters = 0;
};

// Save bytes on the real filesystem. Only the follow-up result for the selected
// document is injected, so the real Workbench close-warning guard declines once.
class SaveWarningFileSystem final : public io::IFileSystem {
public:
    Result<std::string> readFile(std::string_view path) override { return m_native.readFile(path); }
    Result<std::string> readFileSnapshot(std::string_view path, std::size_t maxBytes) override {
        return m_native.readFileSnapshot(path, maxBytes);
    }
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_native.writeFileAtomic(path, data);
    }
    Result<io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                          const io::SafeWriteOptions& options) override {
        auto result = m_native.safeWrite(path, data, options);
        if (result && path.ends_with("veto-save.txt"))
            result->warning = Error(ErrorCode::IoError, "fixture recovery metadata needs attention");
        return result;
    }
    Result<io::FileStatus> status(std::string_view path) override { return m_native.status(path); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view path) override {
        return m_native.listDirectory(path);
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        return m_native.listDirectoryWithDiagnostics(path);
    }
    Result<void> createDirectories(std::string_view path) override { return m_native.createDirectories(path); }
    Result<void> removeFile(std::string_view path) override { return m_native.removeFile(path); }
    Result<void> removeDirectory(std::string_view path) override { return m_native.removeDirectory(path); }
    Result<void> rename(std::string_view from, std::string_view to) override { return m_native.rename(from, to); }
    Result<bool> exists(std::string_view path) override { return m_native.exists(path); }
    Result<std::string> canonicalPath(std::string_view path) override { return m_native.canonicalPath(path); }
    Result<std::string> physicalPath(std::string_view path) override { return m_native.physicalPath(path); }

private:
    io::IFileSystem& m_native = io::realFileSystem();
};

Result<update::AuthenticatedRelease> signFixture(std::span<const std::byte> payload) {
    if (sodium_init() < 0)
        return fail(ErrorCode::IoError, "Could not initialize fixture signing");
    update::Sha256 hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    std::array<char, 65> digest{};
    sodium_bin2hex(digest.data(), digest.size(), hash.data(), hash.size());
    const auto manifest = std::format(
        R"({{"schema":1,"channel":"stable","platform":"windows","arch":"x64","version":"2.0.0",)"
        R"("build":200,"expires":2000,"asset":{{"url":"https://github.com/Frowsty/FrostIDE/releases/download/v2.0.0/Frost.zip",)"
        R"("size":{},"sha256":"{}"}}}})",
        payload.size(), digest.data());
    update::PublicKey key{};
    update::AuthenticatedRelease authenticated{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    if (crypto_sign_keypair(key.data(), secret.data()) != 0 ||
        crypto_sign_detached(authenticated.signature.data(), nullptr,
                             reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size(),
                             secret.data()) != 0)
        return fail(ErrorCode::IoError, "Could not sign the fixture manifest");
    sodium_memzero(secret.data(), secret.size());
    auto manifestBytes = std::as_bytes(std::span(manifest));
    const update::ReleasePolicy policy{"stable", "windows", "x64", 1000, 100, 0, "1.0.0", "", ""};
    auto release = update::verifyRelease(manifestBytes, authenticated.signature, key, policy);
    if (!release)
        return std::unexpected(release.error());
    if (auto verified = update::verifyPayload(payload, static_cast<std::size_t>(release->size), release->digest);
        !verified)
        return std::unexpected(verified.error());
    authenticated.release = std::move(*release);
    authenticated.manifest.assign(manifestBytes.begin(), manifestBytes.end());
    return authenticated;
}

int driveUpdate(const QString& mode, const QString& zip, const QDir& workspace) {
    QFile input(zip);
    if (!input.open(QIODevice::ReadOnly))
        return 2;
    QByteArray bytes = input.readAll();
    auto release = signFixture(std::as_bytes(std::span(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
    if (!release) {
        std::fprintf(stderr, "%s\n", release.error().describe().c_str());
        return 2;
    }
    const bool tampered = mode == QStringLiteral("tampered");
    const bool veto = mode == QStringLiteral("vetoed");
    if (tampered)
        bytes[0] = '!';
    auto payload = std::make_unique<QTemporaryFile>(workspace.filePath(QStringLiteral("offer-XXXXXX")));
    if (!payload->open() || payload->write(bytes) != bytes.size() || !payload->flush())
        return 2;
    payload->close();

    SaveWarningFileSystem warningFs;
    io::IFileSystem& fs = veto ? static_cast<io::IFileSystem&>(warningFs) : io::realFileSystem();
    const auto settingsPath = workspace.filePath(QStringLiteral("settings.json")).toStdString();
    auto settings = settings::SettingsStore::open(settings::builtinRegistry(), fs, {settingsPath, std::nullopt});
    auto trust = project::TrustStore::open(fs, workspace.filePath(QStringLiteral("trust.txt")).toStdString());
    if (!settings || !trust)
        return 2;
    if (!(*settings)->set(settings::Scope::Application, settings::key::saveAutosaveOnFrameDeactivation, false) ||
        !(*settings)->set(settings::Scope::Application, settings::key::saveAutosaveOnIdle, false))
        return 2;
    WorkbenchOptions options;
    options.clangd = false;
    options.git = false;
    options.projectHost = false;
    options.followActiveLayout = false;
    options.applicationSettingsFile = settingsPath;
    options.journalDirectory = workspace.filePath(QStringLiteral("journals")).toStdString();
    ObservingLauncher launcher;
    DialogWorkbenchPrompts prompts;
    Workbench workbench(WorkbenchServices{fs, launcher, systemClock(), **settings, **trust, prompts},
                        std::move(options));
    MainWindow window(workbench.environment());
    workbench.attach(window);
    window.show();
    workbench.markInteractiveStartupComplete();

    if (veto) {
        const auto file = workspace.filePath(QStringLiteral("veto-save.txt")).toStdString();
        if (!fs.writeFileAtomic(file, "old") || !workbench.openPath(file))
            return 2;
        auto* editor = workbench.editors().find(file);
        if (!editor || !editor->editor().typeText("user edit"))
            return 2;
        const auto saved = workbench.navigation()->saveAll();
        if (saved.saved.size() != 1 || saved.warnings.size() != 1 || !saved.failed.empty() ||
            fs.readFile(file).value_or("") == "old")
            return 2;
    }

    bool clicked = false;
    bool vetoed = false;
    bool integrityRefused = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible())
                continue;
            if (widget->objectName() == QStringLiteral("updateInstallerDialog") && !clicked) {
                if (auto* button = widget->findChild<QPushButton*>(QStringLiteral("installInPlaceButton"));
                    button && button->isEnabled()) {
                    clicked = true;
                    button->click();
                }
            } else if (auto* box = qobject_cast<QMessageBox*>(widget); box) {
                if (box->windowTitle() == QStringLiteral("Update Installation Failed") && tampered) {
                    integrityRefused = box->text().contains(QStringLiteral("hash"), Qt::CaseInsensitive) ||
                                       box->text().contains(QStringLiteral("digest"), Qt::CaseInsensitive);
                    box->accept();
                }
            }
        }
    });
    driver.start(15);
    QTimer::singleShot(30000, &window, [] { QCoreApplication::exit(8); });
    bool outcome = false;
    QTimer::singleShot(0, &window, [&] {
        WorkbenchUpdateFixture::offer(workbench, std::move(payload), std::move(*release));
        vetoed = std::ranges::any_of(window.notifications().log(), [](const ui::NotificationEntry& entry) {
            return entry.notification.title == QStringLiteral("Review save warnings before closing");
        });
        const bool shouldStayOpen = mode != QStringLiteral("approved");
        const bool modalObserved = (veto && vetoed && !window.tabs().isDirty(0)) || (tampered && integrityRefused);
        outcome = clicked && window.isVisible() == shouldStayOpen && launcher.updaters == (tampered ? 0 : 1) &&
                  (!shouldStayOpen || modalObserved);
        QCoreApplication::exit(outcome ? 0 : 5);
    });
    const int status = QApplication::exec();
    workbench.shutdown();
    std::printf("clicked=%d window_visible=%d helper_launches=%d veto=%d integrity_refused=%d\n", clicked,
                window.isVisible(), launcher.updaters, vetoed, integrityRefused);
    return outcome ? status : 5;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 4)
        return 2;
    QStandardPaths::setTestModeEnabled(true);
    const QDir workspace(arguments[3]);
    QCoreApplication::setOrganizationName(QStringLiteral("FrostIDE-Update-Tests"));
    QCoreApplication::setApplicationName(workspace.dirName());
    QFile profile(workspace.filePath(QStringLiteral("profile-path.txt")));
    if (!profile.open(QIODevice::WriteOnly) ||
        profile.write(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toUtf8()) <= 0)
        return 2;
    profile.close();
    return driveUpdate(arguments[1], arguments[2], workspace);
}
