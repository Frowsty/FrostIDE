// SPDX-License-Identifier: Apache-2.0
// Project-scoped IPC request validation and GUI-thread dispatch for external script opens.
#include "project_open_broker.hpp"

#include "frost/app/main_window.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <climits>
#include <condition_variable>
#include <mutex>

namespace frost::app {
namespace {
using namespace std::chrono_literals;
std::string utf8(const QString& text) {
    auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
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
        const QString canonicalFile = info.canonicalFilePath();
        if (canonicalFile.isEmpty())
            return fail(ErrorCode::IoError, "cannot resolve project file");
        info = QFileInfo(QFileInfo(canonicalFile).absolutePath());
    }
    if (!info.isDir())
        return fail(ErrorCode::InvalidArgument, "project must be a folder or .frostproj file");
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return fail(ErrorCode::IoError, "cannot resolve project path");
    return utf8(canonical);
}
std::string externalProjectKey(std::string_view root) {
    const std::string key = lang::documentKey(root);
    return QCryptographicHash::hash(QByteArray(key.data(), static_cast<qsizetype>(key.size())),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
Result<void> openExternalFile(Workbench& workbench, std::string_view path, int line, int column) {
    if (path.empty())
        return {};
    if (!io::isAbsolutePath(path) || path.find('\0') != std::string_view::npos || line < 0 || column < 0)
        return fail(ErrorCode::InvalidArgument, "invalid external file or location");
    const QFileInfo requested(qs(path));
    const QString canonicalPath = requested.canonicalFilePath();
    const QFileInfo target(canonicalPath);
    if (canonicalPath.isEmpty() || !target.isFile() ||
        target.suffix().compare(QStringLiteral("frostproj"), Qt::CaseInsensitive) == 0 ||
        target.suffix().compare(QStringLiteral("sln"), Qt::CaseInsensitive) == 0 ||
        target.suffix().compare(QStringLiteral("slnx"), Qt::CaseInsensitive) == 0)
        return fail(ErrorCode::InvalidArgument, "external file must be an existing editor document");
    const std::string canonical = utf8(canonicalPath);
    // An editor opened through a symlink can already hold unsaved changes. Reopen its
    // original path to select that tab instead of creating a second document for the target.
    std::string editorPath = canonical;
    for (auto* view : workbench.editors().views()) {
        const QString existing = QFileInfo(qs(view->path())).canonicalFilePath();
        if (!existing.isEmpty() && lang::documentKey(utf8(existing)) == lang::documentKey(canonical)) {
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
        view->goToLine(line > 0 ? line : status.line, column > 0 ? column : status.column);
    }
    return {};
}
Result<void> forwardExternalOpen(std::string_view root, std::string_view file, int line, int column) {
    auto payload = lang::Json::object()
                       .set("root", std::string(root))
                       .set("file", std::string(file))
                       .set("line", line)
                       .set("column", column);
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
                Result<std::string> result =
                    self ? self->accept(std::move(request)) : fail(ErrorCode::Cancelled, "window closed");
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
Result<std::string> ProjectOpenBroker::accept(std::string request) {
    auto json = lang::parseJson(request);
    if (!json || !json->isObject())
        return fail(ErrorCode::InvalidArgument, "invalid open request");
    const std::string root(json->stringAt("root"));
    const std::string file(json->stringAt("file"));
    auto line = json->integerAt("line");
    auto column = json->integerAt("column");
    auto requestedRoot = externalProjectRoot(root);
    auto currentRoot = externalProjectRoot(m_workbench.projectRoot());
    if (!line || !column || *line < 0 || *column < 0 || *line > INT_MAX || *column > INT_MAX || !requestedRoot ||
        !currentRoot || lang::documentKey(*requestedRoot) != lang::documentKey(m_root) ||
        lang::documentKey(*currentRoot) != lang::documentKey(m_root))
        return fail(ErrorCode::InvalidArgument, "request does not match this window's project");
    if (auto opened = openExternalFile(m_workbench, file, static_cast<int>(*line), static_cast<int>(*column)); !opened)
        return std::unexpected(std::move(opened.error()));
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
