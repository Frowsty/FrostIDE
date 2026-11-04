// SPDX-License-Identifier: Apache-2.0
// Workbench integration for trusted C# projects and managed document diagnostics.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "language_bridge.hpp"
#include "unity_context.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::app {
namespace {
sched::Task<void> stopOwnedManaged(std::shared_ptr<lang::ManagedService> server) { co_await server->stop(); }
// Opens only regular files and never follows a final link. On POSIX every directory
// component is opened relative to its parent with O_NOFOLLOW; O_NONBLOCK prevents a
// FIFO swapped in between lookup and open from stalling the GUI thread.
std::optional<std::string> readUnityControl(const QString& physicalRoot, const std::vector<std::string_view>& parts,
                                            bool readContents) {
    if (parts.empty())
        return std::nullopt;
#ifdef _WIN32
    // Keep every ancestor open without write or delete sharing until the final handle is
    // checked. This prevents a checked directory from being renamed or replaced
    // by a junction between the checks and the final open.
    struct Handles {
        std::vector<HANDLE> values;
        ~Handles() {
            for (HANDLE value : values)
                CloseHandle(value);
        }
    } handles;
    QString path = physicalRoot;
    auto openChecked = [&](const QString& current, bool directory) -> HANDLE {
        auto wide = io::toWin32Path(current.toUtf8().toStdString());
        if (!wide)
            return INVALID_HANDLE_VALUE;
        HANDLE value = CreateFileW(
            wide->c_str(), GENERIC_READ, directory ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE), nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        if (value == INVALID_HANDLE_VALUE)
            return value;
        handles.values.push_back(value);
        BY_HANDLE_FILE_INFORMATION entry{};
        if (GetFileType(value) != FILE_TYPE_DISK || !GetFileInformationByHandle(value, &entry) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
            return INVALID_HANDLE_VALUE;
        return value;
    };
    if (openChecked(path, true) == INVALID_HANDLE_VALUE)
        return std::nullopt;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto part = parts[i];
        path = QDir(path).filePath(QString::fromUtf8(part.data(), static_cast<qsizetype>(part.size())));
        handle = openChecked(path, i + 1 != parts.size());
        if (handle == INVALID_HANDLE_VALUE)
            return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    std::array<wchar_t, 32768> finalPath{};
    const DWORD finalLength =
        GetFinalPathNameByHandleW(handle, finalPath.data(), static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED);
    QString finalName = finalLength && finalLength < finalPath.size()
                            ? QDir::fromNativeSeparators(QString::fromWCharArray(finalPath.data(), finalLength))
                            : QString{};
    if (finalName.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive))
        finalName = QStringLiteral("//") + finalName.sliced(8);
    else if (finalName.startsWith(QStringLiteral("//?/")))
        finalName = finalName.sliced(4);
    const bool insideRoot = !finalName.isEmpty() &&
                            finalName.startsWith(QDir::cleanPath(physicalRoot) + QLatin1Char('/'), Qt::CaseInsensitive);
    const bool regular = insideRoot && GetFileType(handle) == FILE_TYPE_DISK &&
                         GetFileInformationByHandle(handle, &info) &&
                         (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    std::array<char, 69> buffer{};
    DWORD count = 0;
    const bool ok = regular && (!readContents ||
                                ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr));
    if (!ok || count > 68)
        return std::nullopt;
    return readContents ? std::optional<std::string>(std::string(buffer.data(), count)) : std::string{};
#else
    const QByteArray root = physicalRoot.toUtf8();
    int fd = ::open(root.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return std::nullopt;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string name(parts[i]);
        const bool last = i + 1 == parts.size();
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (last ? O_NOCTTY : O_DIRECTORY);
        const int next = ::openat(fd, name.c_str(), flags);
        ::close(fd);
        if (next < 0)
            return std::nullopt;
        fd = next;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        (readContents && (info.st_size < 0 || info.st_size > 68))) {
        ::close(fd);
        return std::nullopt;
    }
    std::array<char, 69> buffer{};
    ssize_t count = 0;
    if (readContents) {
        do {
            count = ::read(fd, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
    }
    ::close(fd);
    if (count < 0 || count > 68)
        return std::nullopt;
    return readContents ? std::optional<std::string>(std::string(buffer.data(), static_cast<std::size_t>(count)))
                        : std::string{};
#endif
}
} // namespace
std::string trustedUnityRevision(const std::string& root) {
    const QDir project(QString::fromUtf8(root.data(), static_cast<qsizetype>(root.size())));
    if (!QFileInfo(project.filePath(QStringLiteral("Assets"))).isDir() ||
        !QFileInfo(project.filePath(QStringLiteral("Packages"))).isDir() ||
        !QFileInfo(project.filePath(QStringLiteral("ProjectSettings"))).isDir())
        return {};
    const QString physicalRoot = project.canonicalPath();
    if (physicalRoot.isEmpty())
        return {};
    auto owner = readUnityControl(physicalRoot, {".frost", "unity", "owner"}, true);
    if (!owner || *owner != "FrostIDE Unity project context v1")
        return {};
    auto revision = readUnityControl(physicalRoot, {".frost", "unity", "revision"}, true);
    if (!revision || revision->size() != 67 || !revision->starts_with("1:") || (*revision)[34] != ':' ||
        !std::ranges::all_of(std::string_view(*revision).substr(2, 32),
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        !std::ranges::all_of(std::string_view(*revision).substr(35),
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return {};
    const std::string generation = revision->substr(2, 32);
    if (!readUnityControl(physicalRoot, {".frost", "unity", "generations", generation, "FrostUnity.sln"}, false))
        return {};
    return generation;
}
namespace {
QString unitySolution(const std::string& root, const std::string& revision) {
    const QDir project(QString::fromUtf8(root.data(), static_cast<qsizetype>(root.size())));
    return project.filePath(QStringLiteral(".frost/unity/generations/") +
                            QString::fromLatin1(revision.data(), static_cast<qsizetype>(revision.size())) +
                            QStringLiteral("/FrostUnity.sln"));
}
} // namespace
bool isManagedSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    auto ext = path.substr(path.size() - 3);
    return ext[0] == '.' && (ext[1] == 'c' || ext[1] == 'C') && (ext[2] == 's' || ext[2] == 'S');
}
void Workbench::offerManagedTrust() {
    if (m_projectRoot.empty() || m_services.trust.isTrusted(m_projectRoot) || !m_window || m_managedTrustDialog)
        return;
    auto* dialog = new ui::TrustDialog(
        *m_build, {QString::fromUtf8(m_projectRoot.data(), static_cast<qsizetype>(m_projectRoot.size()))}, m_window,
        ui::TrustPurpose::ManagedLanguageServer);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_managedTrustDialog = dialog;
    const std::string root = m_projectRoot;
    connect(dialog, &QDialog::accepted, this, [this, root] {
        m_managedTrustDialog.clear();
        if (m_projectRoot == root && m_services.trust.isTrusted(root))
            startManaged(root);
    });
    dialog->open();
}
void Workbench::checkUnityProjectContext() {
    if (m_shutDown || m_projectRoot.empty() || m_projectRoot != m_unityRevisionRoot ||
        !m_services.trust.isTrusted(m_projectRoot))
        return;
    const std::string revision = trustedUnityRevision(m_projectRoot);
    if (revision != m_unityRevision)
        startManaged(m_projectRoot);
}
void Workbench::startManaged(const std::string& root) {
    if (m_shutDown)
        return;
    m_unityRevisionRoot = root;
    m_unityRevision.clear();
    stopManaged();
    const auto snapshot = m_services.settings.snapshot();
    m_managedDotnetPath = snapshot->get(settings::key::toolsManagedDotnetPath);
    m_managedServerPath = snapshot->get(settings::key::toolsManagedServerPath);
    if (!m_services.trust.isTrusted(root)) {
        for (auto* view : m_editors->views()) {
            if (isManagedSource(view->path())) {
                offerManagedTrust();
                break;
            }
        }
        return;
    }
    m_unityRevision = trustedUnityRevision(root);
    std::string dotnetPath = m_managedDotnetPath;
    std::string serverPath = m_managedServerPath;
    if (dotnetPath.empty()) {
#ifdef _WIN32
        const QString executable = QStringLiteral("dotnet.exe");
#else
        const QString executable = QStringLiteral("dotnet");
#endif
        const auto isRegular = [&](const std::string& candidate) {
            if (!io::isAbsolutePath(candidate))
                return false;
            const auto st = m_services.fs.status(candidate);
            return st && st->type == io::FileType::Regular;
        };
        const QString dotnetRoot = qEnvironmentVariable("DOTNET_ROOT");
        if (!dotnetRoot.isEmpty()) {
            const std::string candidate = QDir(dotnetRoot).filePath(executable).toUtf8().toStdString();
            if (isRegular(candidate))
                dotnetPath = candidate;
        }
        if (dotnetPath.empty()) {
            const QString appDir = QCoreApplication::applicationDirPath();
            if (!appDir.isEmpty()) {
                const std::string bundled =
                    QDir(appDir).filePath(QStringLiteral("dotnet/") + executable).toUtf8().toStdString();
                if (isRegular(bundled))
                    dotnetPath = bundled;
            }
        }
        if (dotnetPath.empty()) {
            const QString sdk = qEnvironmentVariable("FROST_SDK");
            if (!sdk.isEmpty()) {
                const std::string sdkDotnet =
                    QDir(sdk).filePath(QStringLiteral("dotnet/") + executable).toUtf8().toStdString();
                if (isRegular(sdkDotnet))
                    dotnetPath = sdkDotnet;
            }
        }
#ifdef _WIN32
        if (dotnetPath.empty()) {
            const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
            const std::string sysDotnet =
                QDir(programFiles).filePath(QStringLiteral("dotnet/dotnet.exe")).toUtf8().toStdString();
            if (isRegular(sysDotnet))
                dotnetPath = sysDotnet;
        }
#else
        if (dotnetPath.empty()) {
            for (const char* dir : {"/usr/share/dotnet", "/usr/local/share/dotnet", "/opt/dotnet"}) {
                const std::string sysDotnet =
                    QDir(QString::fromLatin1(dir)).filePath(executable).toUtf8().toStdString();
                if (isRegular(sysDotnet)) {
                    dotnetPath = sysDotnet;
                    break;
                }
            }
        }
#endif
        if (dotnetPath.empty()) {
            const QString onPath = QStandardPaths::findExecutable(QStringLiteral("dotnet"));
            if (!onPath.isEmpty()) {
                const std::string candidate = onPath.toUtf8().toStdString();
                if (isRegular(candidate))
                    dotnetPath = candidate;
            }
        }
    }
    if (serverPath.empty()) {
        const QString sdk = qEnvironmentVariable("FROST_SDK");
        if (!sdk.isEmpty()) {
            const std::string sdkServer =
                QDir(sdk).filePath(QStringLiteral("managed/CSharpLanguageServer.dll")).toUtf8().toStdString();
            const auto st = m_services.fs.status(sdkServer);
            if (st && st->type == io::FileType::Regular)
                serverPath = sdkServer;
        }
        if (serverPath.empty()) {
            const QString appDir = QCoreApplication::applicationDirPath();
            serverPath =
                QDir(appDir).filePath(QStringLiteral("managed/CSharpLanguageServer.dll")).toUtf8().toStdString();
        }
    }
    if (!io::isAbsolutePath(dotnetPath) || !io::isAbsolutePath(serverPath)) {
        return;
    }
    const auto dotnet = m_services.fs.status(dotnetPath);
    const auto assembly = m_services.fs.status(serverPath);
    if (!dotnet || !assembly || dotnet->type != io::FileType::Regular || assembly->type != io::FileType::Regular) {
        if (!m_managedDotnetPath.empty() || !m_managedServerPath.empty())
            notify(QStringLiteral("C# language server"),
                   QStringLiteral("The configured .NET SDK or server was not found."), ui::NotificationKind::Warning);
        return;
    }
    if (!m_reactor) {
        auto reactor = io::Reactor::create(io::ReactorOptions{1, "frost-managed-io"});
        if (!reactor) {
            log::warn("workbench", "C# language server reactor: {}", reactor.error().describe());
            return;
        }
        m_reactor = std::move(*reactor);
    }
    const QString cliHome = qEnvironmentVariable("FROST_DOTNET_CLI_HOME");
    if (!cliHome.isEmpty() && (!QDir::isAbsolutePath(cliHome) || !QDir().mkpath(cliHome))) {
        notify(QStringLiteral("C# language server"), QStringLiteral("Could not create the configured .NET CLI home."),
               ui::NotificationKind::Warning);
        return;
    }
    auto options = lang::managedServiceOptions(dotnetPath, serverPath, root, true);
    options.environment.emplace_back("DOTNET_ROLL_FORWARD", "LatestMajor");
    // Keep the Unity root as the LSP workspace: source documents live outside the
    // generated directory. Tell csharp-ls which solution to load explicitly.
    if (!m_unityRevision.empty()) {
        const QString solution = unitySolution(root, m_unityRevision);
        options.arguments.emplace_back("--solution");
        options.arguments.push_back(solution.toUtf8().toStdString());
    }
    if (!cliHome.isEmpty())
        options.environment.emplace_back("DOTNET_CLI_HOME", cliHome.toUtf8().toStdString());
    options.onLifecycle = [self = QPointer<Workbench>(this)] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self] {
                    if (self && self->m_language)
                        self->m_language->structureChanged();
                },
                Qt::QueuedConnection);
    };
    m_managed = std::make_shared<lang::ManagedService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                       std::move(options));
    m_managedListener = m_managed->diagnostics().addListener([this](const lang::FileDiagnostics& data) {
        QMetaObject::invokeMethod(this, [this, file = data.file] { onManagedDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_managed->start(); !started) {
        log::warn("workbench", "C# language server startup: {}", started.error().describe());
        stopManaged();
        return;
    }
    m_bridge->setManagedServer(m_managed);
    for (auto* view : m_editors->uniqueDocumentViews())
        syncManagedDocument(*view);
    updateLanguageTarget();
    m_language->structureChanged();
}
void Workbench::stopManaged() {
    if (m_managedTrustDialog) {
        m_managedTrustDialog->close();
        m_managedTrustDialog.clear();
    }
    if (!m_managed)
        return;
    m_language->editorChanged();
    m_bridge->setManagedServer(nullptr);
    m_managed->diagnostics().removeListener(m_managedListener);
    m_managedListener = 0;
    for (auto* view : m_editors->views()) {
        if (!isManagedSource(view->path()))
            continue;
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setInsightEnabled(false);
        m_liveMarkers.erase(lang::documentKey(view->path()));
        refreshMarkers(*view);
    }
    auto old = std::move(m_managed);
    m_retiredManaged.push_back(m_pool->spawn(sched::Lane::Background, stopOwnedManaged(std::move(old))));
    refreshLiveProblems();
}
void Workbench::syncManagedDocument(DocumentEditorView& view) {
    if (!m_managed || !isManagedSource(view.path()))
        return;
    const auto& document = view.document().document();
    if (m_managed->openDocument(view.path(), document.text())) {
        view.editor().setInsightEnabled(true);
        return;
    }
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_managed->changeDocument(view.path(), whole, document.text());
}
void Workbench::onManagedDiagnostics(const std::string& file) {
    if (!m_managed || !isManagedSource(file))
        return;
    auto set = m_managed->diagnostics().forFile(file);
    if (set && set->stale)
        return;
    std::vector<ui::LineMarker> markers;
    if (set)
        for (const auto& d : set->diagnostics) {
            if (d.line <= 0 ||
                (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning))
                continue;
            markers.push_back({LineIndex{static_cast<std::uint32_t>(d.line - 1)},
                               d.severity == lang::DiagnosticSeverity::Error ? ui::MarkerSeverity::Error
                                                                             : ui::MarkerSeverity::Warning,
                               d.message});
        }
    const auto key = lang::documentKey(file);
    if (markers.empty())
        m_liveMarkers.erase(key);
    else
        m_liveMarkers[key] = std::move(markers);
    for (DocumentEditorView* view : m_editors->views()) {
        if (lang::documentKey(view->path()) != key)
            continue;
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                      set ? ui::lang::toEditorDiagnostics(*set) : std::vector<ui::EditorDiagnostic>{});
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}
} // namespace frost::app
