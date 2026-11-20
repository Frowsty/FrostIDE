// SPDX-License-Identifier: Apache-2.0
// Physical current-workspace resource allowlist for the consent-scoped C1 editor server.
#include "agent_scope.hpp"

#include "frost/io/path.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
namespace frost::app {
Result<std::string> agentWorkspaceResource(io::IFileSystem& fs, std::string_view root, std::string_view resource) {
    if (resource.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "invalid agent resource");
    QString input = QString::fromUtf8(resource.data(), static_cast<qsizetype>(resource.size()));
#ifdef _WIN32
    // WSL mirrored networking keeps the loopback-only listener; map only drive-mounted files.
    // Physical root containment below still rejects every resource outside this window's root.
    if (input.size() >= 7 && input.startsWith("/mnt/") && input[5].isLetter() && input[6] == '/')
        input = input[5].toUpper() + QStringLiteral(":") + input.mid(6);
#endif
    const QUrl url(input);
    const auto path = (url.isLocalFile() ? url.toLocalFile() : input).toUtf8().toStdString();
    if (!io::isAbsolutePath(path))
        return fail(ErrorCode::InvalidArgument, "agent resource must be absolute");
    const auto denied = [](std::string_view name) {
        const auto leaf = QFileInfo(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()))).fileName();
        return leaf.compare(".credentials.json", Qt::CaseInsensitive) == 0 ||
               leaf.contains("keychain", Qt::CaseInsensitive);
    };
    if (denied(path))
        return fail(ErrorCode::PermissionDenied, "credential resources are unavailable to the editor bridge");
    const auto physicalRoot = fs.physicalPath(root);
    const auto physical = fs.physicalPath(path);
    if (!physicalRoot || !physical)
        return fail(ErrorCode::NotFound, "agent resource is unavailable");
    const auto relative =
        QDir(QString::fromUtf8(physicalRoot->c_str())).relativeFilePath(QString::fromUtf8(physical->c_str()));
    if (denied(*physical) || relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
        return fail(ErrorCode::PermissionDenied, "agent resource is outside the consenting workspace");
    const auto status = fs.status(*physical);
    if (!status || status->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "agent resource must be a regular file");
    return *physical;
}
} // namespace frost::app
