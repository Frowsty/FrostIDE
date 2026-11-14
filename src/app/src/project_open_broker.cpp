// SPDX-License-Identifier: Apache-2.0
// Project-scoped IPC request validation and GUI-thread dispatch for external script opens.
#include "project_open_broker.hpp"

#include "frost/app/main_window.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::app {
namespace {
using namespace std::chrono_literals;
std::string utf8(const QString& text) {
    auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
Result<std::string> physicalInfoPath(const QFileInfo& info) {
    return io::realFileSystem().physicalPath(utf8(info.absoluteFilePath()));
}
struct Waiter {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    Result<std::string> result = fail(ErrorCode::Timeout, "window did not accept request in time");
};
} // namespace
Result<std::string> externalProjectRoot(std::string_view project) {
    if (project.empty() || project.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "invalid project path");
    QFileInfo info(qs(project));
    if (!info.exists())
        return fail(ErrorCode::NotFound, "project path does not exist");
    if (info.isFile()) {
        if (info.suffix().compare(QStringLiteral("frostproj"), Qt::CaseInsensitive) != 0)
            return fail(ErrorCode::InvalidArgument, "project must be a folder or .frostproj file");
        // Resolve the project file itself before taking its parent: a symlink may point to
        // a different directory than the link. Workbench opens that same canonical file.
        const auto canonicalFile = physicalInfoPath(info);
        if (!canonicalFile)
            return std::unexpected(canonicalFile.error());
        info = QFileInfo(QFileInfo(qs(*canonicalFile)).absolutePath());
    }
    if (!info.isDir())
        return fail(ErrorCode::InvalidArgument, "project must be a folder or .frostproj file");
    // Broker identity follows directory junctions independently of editor spelling.
    return physicalInfoPath(info);
}
std::optional<std::string> inferExternalProjectRootForFile(std::string_view filePath) {
    if (filePath.empty() || filePath.find('\0') != std::string_view::npos)
        return std::nullopt;
    const QFileInfo fileInfo(qs(filePath));
    if (!fileInfo.exists() || !fileInfo.isFile())
        return std::nullopt;
    const QString suffix = fileInfo.suffix().toLower();
    if (suffix == QStringLiteral("frostproj") || suffix == QStringLiteral("sln") || suffix == QStringLiteral("slnx") ||
        fileInfo.fileName().compare(QStringLiteral("CMakeLists.txt"), Qt::CaseInsensitive) == 0 ||
        fileInfo.fileName() == QStringLiteral("compile_commands.json") ||
        fileInfo.fileName() == QStringLiteral("Makefile") || fileInfo.fileName() == QStringLiteral("makefile") ||
        fileInfo.fileName() == QStringLiteral("GNUmakefile"))
        return std::nullopt;
    const QString canonicalFile = fileInfo.canonicalFilePath();
    if (canonicalFile.isEmpty())
        return std::nullopt;

    struct AncestorDir {
        QString path;
        QString childName;
    };
    std::vector<AncestorDir> chain;
    QDir current(QFileInfo(canonicalFile).absolutePath());
    QString childName;
    for (int depth = 0; depth < 24 && !current.isRoot(); ++depth) {
        const QString canonicalDir = QFileInfo(current.absolutePath()).canonicalFilePath();
        const QString dirPath = canonicalDir.isEmpty() ? current.absolutePath() : canonicalDir;
        if (dirPath.isEmpty())
            break;
        chain.push_back(AncestorDir{dirPath, childName});
        childName = current.dirName();
        if (!current.cdUp())
            break;
    }
    if (chain.empty())
        return std::nullopt;

    const auto hasEntryWithSuffix = [](const QDir& dir, QStringView ext) {
        const auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        return std::ranges::any_of(
            entries, [&](const QFileInfo& entry) { return entry.suffix().compare(ext, Qt::CaseInsensitive) == 0; });
    };

    // Pass 1: Explicit FrostEngine project root (.frostproj file or .frost directory).
    for (const auto& ancestor : chain) {
        const QDir dir(ancestor.path);
        if (hasEntryWithSuffix(dir, u"frostproj") || QFileInfo(dir.filePath(QStringLiteral(".frost"))).isDir()) {
            if (auto root = externalProjectRoot(utf8(ancestor.path)))
                return *root;
        }
    }

    // Pass 2: Parent directory of an "Assets" folder (FrostEngine / Unity layout: <Root>/Assets/...).
    for (const auto& ancestor : chain) {
        if (ancestor.childName.compare(QStringLiteral("Assets"), Qt::CaseInsensitive) == 0) {
            if (auto root = externalProjectRoot(utf8(ancestor.path)))
                return *root;
        }
    }

    // Pass 3: Workspace markers (.luarc.json, ProjectSettings, .sln, .slnx, CMakeLists.txt).
    for (const auto& ancestor : chain) {
        const QDir dir(ancestor.path);
        if (QFileInfo(dir.filePath(QStringLiteral(".luarc.json"))).isFile() ||
            QFileInfo(dir.filePath(QStringLiteral("ProjectSettings"))).isDir() || hasEntryWithSuffix(dir, u"sln") ||
            hasEntryWithSuffix(dir, u"slnx") || QFileInfo(dir.filePath(QStringLiteral("CMakeLists.txt"))).isFile() ||
            QFileInfo(dir.filePath(QStringLiteral("compile_commands.json"))).isFile() ||
            QFileInfo(dir.filePath(QStringLiteral("Makefile"))).isFile() ||
            QFileInfo(dir.filePath(QStringLiteral("makefile"))).isFile() ||
            QFileInfo(dir.filePath(QStringLiteral("GNUmakefile"))).isFile()) {
            if (auto root = externalProjectRoot(utf8(ancestor.path)))
                return *root;
        }
    }

    // Pass 4: Parent directory of a "Scripts" folder (e.g. <Root>/Scripts/foo.lua).
    for (const auto& ancestor : chain) {
        if (ancestor.childName.compare(QStringLiteral("Scripts"), Qt::CaseInsensitive) == 0) {
            if (auto root = externalProjectRoot(utf8(ancestor.path)))
                return *root;
        }
    }

    // Pass 5: A .lua script directly in a non-root project folder.
    if (suffix == QStringLiteral("lua")) {
        if (auto root = externalProjectRoot(utf8(chain.front().path)))
            return *root;
    }

    return std::nullopt;
}
std::string externalProjectKey(std::string_view root) {
    const std::string key = lang::documentKey(root);
    return QCryptographicHash::hash(QByteArray(key.data(), static_cast<qsizetype>(key.size())),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
Result<void> openExternalFile(Workbench& workbench, std::string_view path, int line, int column, bool columnUtf16) {
    if (path.empty())
        return {};
    if (!io::isAbsolutePath(path) || path.find('\0') != std::string_view::npos || line < 0 || column < 0)
        return fail(ErrorCode::InvalidArgument, "invalid external file or location");
    const QFileInfo requested(qs(path));
    const auto canonicalPath = physicalInfoPath(requested);
    const QFileInfo target(canonicalPath ? qs(*canonicalPath) : QString{});
    if (!canonicalPath || !target.isFile() ||
        target.suffix().compare(QStringLiteral("frostproj"), Qt::CaseInsensitive) == 0 ||
        target.suffix().compare(QStringLiteral("sln"), Qt::CaseInsensitive) == 0 ||
        target.suffix().compare(QStringLiteral("slnx"), Qt::CaseInsensitive) == 0)
        return fail(ErrorCode::InvalidArgument, "external file must be an existing editor document");
    const std::string canonical = *canonicalPath;
    // An editor opened through a symlink or directory junction can already hold unsaved changes. Reopen its
    // original path to select that tab instead of creating a second document for the target.
    std::string editorPath = canonical;
    for (auto* view : workbench.editors().views()) {
        const auto existing = physicalInfoPath(QFileInfo(qs(view->path())));
        if (existing && lang::documentKey(*existing) == lang::documentKey(canonical)) {
            editorPath = view->path();
            break;
        }
    }
    auto opened = workbench.openPath(editorPath);
    if (!opened)
        return opened;
    if (line || column) {
        auto* view = workbench.editors().find(editorPath);
        if (!view)
            return fail(ErrorCode::NotFound, "opened file has no editor");
        const auto status = view->status();
        int targetLine = line > 0 ? line : status.line;
        int targetColumn = column > 0 ? column : status.column;
        if (columnUtf16 && column > 0) {
            // The target may be dirty: convert using the live snapshot, not the file on disk.
            const auto& snapshot = view->document().document().text();
            const auto lineIndex = std::min(static_cast<std::uint32_t>(targetLine - 1), snapshot.lineCount() - 1);
            auto offset = snapshot.offsetAt(
                text::Utf16Point{LineIndex{lineIndex}, Utf16Column{static_cast<std::uint32_t>(column - 1)}});
            if (!offset)
                return std::unexpected(std::move(offset.error()));
            targetLine = static_cast<int>(lineIndex + 1);
            targetColumn = static_cast<int>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(snapshot.pointAt(*offset).column.value()) + 1, INT_MAX));
        }
        view->goToLine(targetLine, targetColumn);
    }
    return {};
}
Result<void> forwardExternalOpen(std::string_view root, std::string_view file, int line, int column, bool columnUtf16) {
    auto payload = lang::Json::object().set("root", std::string(root)).set("file", std::string(file));
    if (columnUtf16) {
        // A legacy owner requires top-level line and column, so it rejects this versioned
        // position instead of silently treating UTF-16 units as bytes.
        payload.set(
            "position",
            lang::Json::object().set("version", 1).set("encoding", "utf-16").set("line", line).set("column", column));
    } else {
        payload.set("line", line).set("column", column);
    }
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    auto reply = io::requestLocalIpc(externalProjectKey(root), payload.dump(), 5s);
    if (!reply)
        return std::unexpected(std::move(reply.error()));
    return {};
}
ProjectOpenBroker::ProjectOpenBroker(Workbench& workbench, MainWindow& window, std::string root,
                                     std::unique_ptr<io::LocalIpcOwner> owner)
    : m_workbench(workbench), m_window(window), m_root(std::move(root)), m_owner(std::move(owner)) {}
ProjectOpenBroker::~ProjectOpenBroker() {
    if (m_owner)
        m_owner->stop();
}
Result<void> ProjectOpenBroker::start() {
    m_firstFrame = true;
    if (!m_owner)
        return {};
    QPointer<ProjectOpenBroker> self(this);
    return m_owner->start([self](std::string request) -> Result<std::string> {
        auto waiter = std::make_shared<Waiter>();
        if (!self)
            return fail(ErrorCode::Cancelled, "window closed");
        bool queued = QMetaObject::invokeMethod(
            self.data(),
            [self, waiter, request = std::move(request)]() mutable {
                {
                    std::lock_guard lock(waiter->mutex);
                    if (waiter->done)
                        return;
                }
                Result<std::string> result = self ? self->accept(request) : fail(ErrorCode::Cancelled, "window closed");
                {
                    std::lock_guard lock(waiter->mutex);
                    waiter->result = std::move(result);
                    waiter->done = true;
                }
                waiter->changed.notify_one();
            },
            Qt::QueuedConnection);
        if (!queued)
            return fail(ErrorCode::IoError, "cannot queue open request");
        std::unique_lock lock(waiter->mutex);
        if (!waiter->changed.wait_for(lock, 2s, [&] { return waiter->done; })) {
            waiter->done = true;
            return fail(ErrorCode::Timeout, "window is busy");
        }
        return std::move(waiter->result);
    });
}
Result<std::string> ProjectOpenBroker::accept(const std::string& request) {
    auto json = lang::parseJson(request);
    if (!json || !json->isObject())
        return fail(ErrorCode::InvalidArgument, "invalid open request");
    if (json->stringAt("method") == "unity.hello") {
        const std::string announcedRoot(json->stringAt("root"));
        if (announcedRoot.empty() || m_workbench.projectRoot().empty() ||
            lang::documentKey(announcedRoot) != lang::documentKey(m_root))
            return fail(ErrorCode::InvalidArgument, "Unity session does not match this window's project");
        if (auto accepted = m_workbench.announceUnity(*json); !accepted)
            return std::unexpected(std::move(accepted.error()));
        return std::string("ok");
    }
    const std::string root(json->stringAt("root"));
    const std::string file(json->stringAt("file"));
    auto line = json->integerAt("line");
    auto column = json->integerAt("column");
    bool columnUtf16 = false;
    if (const auto* position = json->find("position")) {
        if (!position->isObject() || json->find("line") || json->find("column") ||
            position->integerAt("version") != 1 || position->stringAt("encoding") != "utf-16")
            return fail(ErrorCode::InvalidArgument, "invalid versioned position");
        line = position->integerAt("line");
        column = position->integerAt("column");
        if (!column || *column <= 0 || file.empty())
            return fail(ErrorCode::InvalidArgument, "UTF-16 column requires a file and positive column");
        columnUtf16 = true;
    }
    auto requestedRoot = externalProjectRoot(root);
    auto currentRoot = externalProjectRoot(m_workbench.projectRoot());
    if (!line || !column || *line < 0 || *column < 0 || *line > INT_MAX || *column > INT_MAX || !requestedRoot ||
        !currentRoot || lang::documentKey(*requestedRoot) != lang::documentKey(m_root) ||
        lang::documentKey(*currentRoot) != lang::documentKey(m_root))
        return fail(ErrorCode::InvalidArgument, "request does not match this window's project");
    if (auto opened =
            openExternalFile(m_workbench, file, static_cast<int>(*line), static_cast<int>(*column), columnUtf16);
        !opened)
        return std::unexpected(std::move(opened.error()));
    if (m_window.isMinimized())
        m_window.setWindowState((m_window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    m_window.show();
    m_window.raise();
    m_window.activateWindow();
    return std::string("ok");
}
void ProjectOpenBroker::projectChanged() {
    const auto resolved = externalProjectRoot(m_workbench.projectRoot());
    const std::string root = resolved ? *resolved : std::string{};
    if (lang::documentKey(root) == lang::documentKey(m_root))
        return;
    if (m_owner) {
        // Closing a busy client may take its deadline. Retire off the GUI thread; the owner
        // reservation remains held by the worker until its listener is completely stopped.
        m_retired.emplace_back([old = std::move(m_owner)] { old->stop(); });
    }
    m_root = root;
    if (m_root.empty())
        return;
    claimCurrent(30);
}
void ProjectOpenBroker::claimCurrent(unsigned attempts) {
    if (m_root.empty() || m_owner)
        return;
    auto owner = io::LocalIpcOwner::claim(externalProjectKey(m_root));
    if (owner) {
        m_owner = std::move(*owner);
        if (m_firstFrame)
            static_cast<void>(start());
        return;
    }
    // A previous listener may be finishing an in-flight request during a rapid switch back.
    // Retry briefly without blocking the UI; a different owning window remains authoritative.
    if (owner.error().code() == ErrorCode::Busy && attempts > 1) {
        const std::string expected = m_root;
        QTimer::singleShot(100, this, [this, expected, attempts] {
            if (m_root == expected)
                claimCurrent(attempts - 1);
        });
    }
}
} // namespace frost::app
