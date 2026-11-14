// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): Solution Explorer file operations, rename and delete (with
// their prompts and FrostEngine .fmeta sidecars), copy, cut, paste, move and duplicate (split from
// workbench_project.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_properties_dialog.hpp"
#include "unity_control.hpp"
#include "workbench_project_helpers.hpp"
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMimeData>
#include <QUrl>

#include <array>
#include <format>
#include <utility>

namespace frost::app {

namespace {

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

void Workbench::checkFrostEngineAssetReferences(std::string_view path, std::string_view operation) {
    m_lastAssetReferenceReport.reset();
    const auto assetLoc = lang::locateFrostEngineAsset(m_services.fs, path);
    if (!assetLoc) {
        return;
    }
    const bool trusted = m_services.trust.isTrusted(assetLoc->projectRoot) &&
                         (m_projectRoot.empty() || m_services.trust.isTrusted(m_projectRoot));
    if (!trusted) {
        return;
    }
    const auto editorExe =
        lang::findFrostEditorExecutable(m_services.fs, assetLoc->projectRoot, m_options.frostEditorPath);
    if (!editorExe) {
        return;
    }
    lang::AssetFindReferencesRequest request;
    request.executablePath = *editorExe;
    request.projectPath = assetLoc->projectRoot;
    request.assetPath = assetLoc->assetPath;
    request.trusted = trusted;
    auto report = lang::runAssetFindReferences(m_services.launcher, request);
    if (!report) {
        return;
    }
    if (report->hasWarnings()) {
        notify(text("Asset references for %1").arg(qs(io::fileName(path))), qs(report->formatWarningSummary(operation)),
               ui::NotificationKind::Warning);
    }
    m_lastAssetReferenceReport = std::move(*report);
}

/*
 * Executes `plan` on disk and, when a `.fmeta` sidecar moved with the asset, rebinds open tabs of
 * the sidecar to its new path.
 */
Result<void> Workbench::moveWithSidecar(const lang::FmetaMovePlan& plan) {
    auto moved = lang::executeFmetaMove(m_services.fs, plan);
    if (!moved) {
        return std::unexpected(std::move(moved.error()));
    }
    if (moved->hasSidecar()) {
        rebindRenamedDocument(m_documentRegistry->canonicalize(*moved->sourceSidecarPath),
                              m_documentRegistry->canonicalize(*moved->targetSidecarPath));
    }
    return {};
}

Result<std::string> Workbench::renameItem(std::string_view oldPath, std::string_view newRelativeName) {
    if (oldPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "select a file or folder to rename");
    }
    if (Result<void> validName = validateProjectNameComponent(newRelativeName); !validName) {
        notify(text("Cannot rename"), qs(validName.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(validName.error()));
    }
    std::string canonicalOld = m_documentRegistry->canonicalize(oldPath);
    if (!m_projectRoot.empty() && lang::documentKey(canonicalOld) == lang::documentKey(m_projectRoot)) {
        Error err(ErrorCode::InvalidArgument, "cannot rename the project root folder");
        notify(text("Cannot rename"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonicalOld);
    if (!status) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (lang::isFmetaPath(canonicalOld)) {
        Error err(ErrorCode::InvalidArgument,
                  "cannot rename or move a .fmeta file directly; rename or move the asset itself");
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(err.message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    if (io::fileName(canonicalOld) == newRelativeName) {
        return canonicalOld;
    }
    const std::string parentDir(io::parentPath(canonicalOld));
    auto targetJoined = io::joinPath(parentDir, newRelativeName);
    if (!targetJoined) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(targetJoined.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(targetJoined.error()));
    }
    if (m_services.fs.exists(*targetJoined).value_or(false)) {
        Error err(ErrorCode::AlreadyExists, "'" + std::string(newRelativeName) + "' already exists");
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(err.message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    auto movePlan = lang::planFmetaMove(m_services.fs, canonicalOld, *targetJoined);
    if (!movePlan) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(movePlan.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(movePlan.error()));
    }
    if (!isDir) {
        checkFrostEngineAssetReferences(canonicalOld, "Renaming");
    }
    if (auto renamed = moveWithSidecar(*movePlan); !renamed) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonicalOld))), qs(renamed.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(renamed.error()));
    }
    std::string canonicalNew = m_documentRegistry->canonicalize(*targetJoined);

    if (!isDir) {
        rebindRenamedDocument(canonicalOld, canonicalNew);
    } else {
        const std::string oldDirKey = lang::documentKey(canonicalOld);
        std::vector<std::pair<std::string, std::string>> docRenames;
        for (const auto& doc : m_documents->documents()) {
            if (doc->untitled()) {
                continue;
            }
            const std::string docKey = lang::documentKey(doc->path());
            if (docKey.starts_with(oldDirKey) && docKey.size() > oldDirKey.size() &&
                (doc->path()[canonicalOld.size()] == '/' || doc->path()[canonicalOld.size()] == '\\')) {
                const std::string suffix = doc->path().substr(canonicalOld.size() + 1);
                if (auto joined = io::joinPath(canonicalNew, suffix); joined) {
                    docRenames.emplace_back(doc->path(), m_documentRegistry->canonicalize(*joined));
                }
            }
        }
        for (const auto& [oldDoc, newDoc] : docRenames) {
            rebindRenamedDocument(oldDoc, newDoc);
        }
        if (m_nav) {
            m_nav->notifyPathRenamed(canonicalOld, canonicalNew);
        }
    }

    renameSourceInCMakeLists(canonicalOld, canonicalNew, isDir);
    updatePathInVcxproj(canonicalOld, canonicalNew, isDir, false);
    refreshSolutionExplorerForFsChange(isDir ? std::string_view{} : std::string_view(canonicalNew));
    return canonicalNew;
}

Result<void> Workbench::deleteItem(std::string_view path) {
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "select a file or folder to delete");
    }
    const std::string canonical = m_documentRegistry->canonicalize(path);
    if (!m_projectRoot.empty() && lang::documentKey(canonical) == lang::documentKey(m_projectRoot)) {
        Error err(ErrorCode::InvalidArgument, "cannot delete the project root folder");
        notify(text("Cannot delete"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    if (m_solution && lang::documentKey(canonical) == lang::documentKey(m_solution->path())) {
        Error err(ErrorCode::InvalidArgument, "cannot delete the open solution file");
        notify(text("Cannot delete"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    auto deletePlan = lang::planFmetaDelete(m_services.fs, canonical);
    if (!deletePlan) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(deletePlan.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(deletePlan.error()));
    }
    if (!isDir) {
        checkFrostEngineAssetReferences(canonical, "Deleting");
    }
    if (auto deleted = lang::executeFmetaDelete(m_services.fs, *deletePlan); !deleted) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(deleted.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(deleted.error()));
    }

    closeDeletedDocuments(canonical, isDir);
    if (deletePlan->hasSidecar()) {
        closeDeletedDocuments(m_documentRegistry->canonicalize(*deletePlan->sidecarPath), false);
    }
    removeSourceFromCMakeLists(canonical, isDir);
    updatePathInVcxproj(canonical, {}, isDir, true);
    refreshSolutionExplorerForFsChange();
    return {};
}

void Workbench::promptAndRenameItem(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    const std::string canonical = m_documentRegistry->canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return;
    }
    if (lang::isFmetaPath(canonical)) {
        notify(text("Cannot rename %1").arg(qs(io::fileName(canonical))),
               text("Cannot rename or move a .fmeta file directly; rename or move the asset itself."),
               ui::NotificationKind::Error);
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    const std::string newName = m_services.prompts.chooseRenameName(m_window, canonical, isDir);
    if (newName.empty() || newName == io::fileName(canonical)) {
        return;
    }
    (void)renameItem(canonical, newName);
}

void Workbench::promptAndDeleteItem(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    const std::string canonical = m_documentRegistry->canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot delete %1").arg(qs(io::fileName(canonical))), qs(status.error().message()),
               ui::NotificationKind::Error);
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (!m_services.prompts.confirmDelete(m_window, qs(io::fileName(canonical)), isDir)) {
        return;
    }
    (void)deleteItem(canonical);
}

namespace {

Result<void> copyFileOrDirectoryRecursiveFs(io::IFileSystem& fs, const std::string& srcPath,
                                            const std::string& dstPath) {
    auto st = fs.status(srcPath);
    if (!st) {
        return std::unexpected(st.error());
    }
    if (st->type == io::FileType::Regular) {
        auto bytes = fs.readFile(srcPath);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return fs.writeFileAtomic(dstPath, *bytes);
    }
    if (st->type == io::FileType::Directory) {
        if (auto created = fs.createDirectories(dstPath); !created) {
            return std::unexpected(created.error());
        }
        auto entries = fs.listDirectory(srcPath);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        for (const io::DirEntry& entry : *entries) {
            auto childSrc = io::joinPath(srcPath, entry.name);
            auto childDst = io::joinPath(dstPath, entry.name);
            if (!childSrc || !childDst) {
                continue;
            }
            if (auto copied = copyFileOrDirectoryRecursiveFs(fs, *childSrc, *childDst); !copied) {
                return std::unexpected(copied.error());
            }
        }
        return {};
    }
    return {};
}

std::string findNonCollidingCopyPath(io::IFileSystem& fs, const std::string& targetDirectory, std::string_view fileName,
                                     bool isDirectory) {
    if (auto direct = io::joinPath(targetDirectory, fileName); direct && !fs.exists(*direct).value_or(false)) {
        return *direct;
    }
    std::string stem(fileName);
    std::string ext;
    if (!isDirectory) {
        if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) {
            ext = stem.substr(dot);
            stem = stem.substr(0, dot);
        }
    }
    for (int idx = 1; idx < 1000; ++idx) {
        const std::string candidateName =
            idx == 1 ? std::format("{}_copy{}", stem, ext) : std::format("{}_copy{}{}", stem, idx, ext);
        if (auto joined = io::joinPath(targetDirectory, candidateName); joined && !fs.exists(*joined).value_or(false)) {
            return *joined;
        }
    }
    return {};
}

} // namespace

void Workbench::copyPathsToClipboard(const std::vector<std::string>& paths) {
    m_clipboardPaths.clear();
    QList<QUrl> urls;
    QStringList textLines;
    for (const std::string& p : paths) {
        if (p.empty()) {
            continue;
        }
        const std::string canonical = m_documentRegistry->canonicalize(p);
        m_clipboardPaths.push_back(canonical);
        const QString qpath = qs(canonical);
        urls.push_back(QUrl::fromLocalFile(qpath));
        textLines.push_back(QDir::toNativeSeparators(qpath));
    }
    m_clipboardIsCut = false;
    if (!urls.isEmpty() && QGuiApplication::clipboard() != nullptr) {
        auto* mime = new QMimeData();
        mime->setUrls(urls);
        mime->setText(textLines.join(QLatin1Char('\n')));
        QGuiApplication::clipboard()->setMimeData(mime);
    }
}

void Workbench::cutPathsToClipboard(const std::vector<std::string>& paths) {
    copyPathsToClipboard(paths);
    m_clipboardIsCut = !m_clipboardPaths.empty();
}

void Workbench::movePhysicalPathsToDirectory(const std::vector<std::string>& sourcePaths,
                                             const std::string& targetDirectory, const std::string& projectPath) {
    if (sourcePaths.empty() || targetDirectory.empty()) {
        return;
    }
    const std::string canonicalTargetDir = m_documentRegistry->canonicalize(targetDirectory);
    auto dirSt = m_services.fs.status(canonicalTargetDir);
    if (!dirSt || dirSt->type != io::FileType::Directory) {
        return;
    }
    std::string lastMoved;
    std::vector<std::string> movedFiles;
    std::vector<std::string> oldParents;

    for (const std::string& rawSrc : sourcePaths) {
        const std::string canonicalOld = m_documentRegistry->canonicalize(rawSrc);
        if (canonicalOld.empty()) {
            continue;
        }
        if (!m_projectRoot.empty() && lang::documentKey(canonicalOld) == lang::documentKey(m_projectRoot)) {
            continue;
        }
        auto st = m_services.fs.status(canonicalOld);
        if (!st) {
            continue;
        }
        const bool isDir = st->type == io::FileType::Directory;
        if (lang::isFmetaPath(canonicalOld)) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))),
                   text("Cannot rename or move a .fmeta file directly; rename or move the asset itself."),
                   ui::NotificationKind::Warning);
            continue;
        }
        const std::string oldParent(io::parentPath(canonicalOld));
        if (lang::documentKey(oldParent) == lang::documentKey(canonicalTargetDir)) {
            continue;
        }
        if (isDir) {
            const std::string oldKey = lang::documentKey(canonicalOld);
            const std::string targetKey = lang::documentKey(canonicalTargetDir);
            if (targetKey == oldKey ||
                (targetKey.starts_with(oldKey) && targetKey.size() > oldKey.size() &&
                 (canonicalTargetDir[canonicalOld.size()] == '/' || canonicalTargetDir[canonicalOld.size()] == '\\'))) {
                notify(text("Cannot move folder"), text("Cannot move a folder into itself."),
                       ui::NotificationKind::Warning);
                continue;
            }
        }
        auto targetJoined = io::joinPath(canonicalTargetDir, io::fileName(canonicalOld));
        if (!targetJoined) {
            continue;
        }
        if (m_services.fs.exists(*targetJoined).value_or(false)) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))),
                   text("'%1' already exists in the destination folder.").arg(qs(io::fileName(canonicalOld))),
                   ui::NotificationKind::Warning);
            continue;
        }
        auto movePlan = lang::planFmetaMove(m_services.fs, canonicalOld, *targetJoined);
        if (!movePlan) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))), qs(movePlan.error().message()),
                   ui::NotificationKind::Warning);
            continue;
        }
        if (!isDir) {
            checkFrostEngineAssetReferences(canonicalOld, "Moving");
        }
        if (auto renamed = moveWithSidecar(*movePlan); !renamed) {
            notify(text("Cannot move %1").arg(qs(io::fileName(canonicalOld))), qs(renamed.error().message()),
                   ui::NotificationKind::Error);
            continue;
        }
        const std::string canonicalNew = m_documentRegistry->canonicalize(*targetJoined);
        oldParents.push_back(oldParent);
        lastMoved = canonicalNew;
        movedFiles.push_back(canonicalNew);

        if (!isDir) {
            rebindRenamedDocument(canonicalOld, canonicalNew);
        } else {
            const std::string oldDirKey = lang::documentKey(canonicalOld);
            std::vector<std::pair<std::string, std::string>> docRenames;
            for (const auto& doc : m_documents->documents()) {
                if (doc->untitled()) {
                    continue;
                }
                const std::string docKey = lang::documentKey(doc->path());
                if (docKey.starts_with(oldDirKey) && docKey.size() > oldDirKey.size() &&
                    (doc->path()[canonicalOld.size()] == '/' || doc->path()[canonicalOld.size()] == '\\')) {
                    const std::string suffix = doc->path().substr(canonicalOld.size() + 1);
                    if (auto joined = io::joinPath(canonicalNew, suffix); joined) {
                        docRenames.emplace_back(doc->path(), m_documentRegistry->canonicalize(*joined));
                    }
                }
            }
            for (const auto& [oldDoc, newDoc] : docRenames) {
                rebindRenamedDocument(oldDoc, newDoc);
            }
            if (m_nav) {
                m_nav->notifyPathRenamed(canonicalOld, canonicalNew);
            }
        }

        renameSourceInCMakeLists(canonicalOld, canonicalNew, isDir);
        updatePathInVcxproj(canonicalOld, canonicalNew, isDir, false);
    }

    if (movedFiles.empty()) {
        return;
    }
    if (!projectPath.empty() && projectPath.ends_with(".vcxproj")) {
        if (auto added = addExistingFilesToVcxproj(m_services.fs, projectPath, movedFiles); !added) {
            notify(text("Moved files were not added to %1").arg(qs(io::fileName(projectPath))),
                   qs(added.error().message()), ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(lastMoved);
        }
    }
    refreshSolutionExplorerForFsChange(lastMoved);
    if (m_solution) {
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
    }
}

void Workbench::pasteClipboardIntoDirectory(const std::string& targetDirectory, const std::string& targetProjectPath,
                                            const std::string& targetFilterPath) {
    if (m_clipboardPaths.empty() || targetDirectory.empty()) {
        return;
    }
    if (m_clipboardIsCut) {
        const std::vector<std::string> toMove = m_clipboardPaths;
        m_clipboardPaths.clear();
        m_clipboardIsCut = false;
        movePhysicalPathsToDirectory(toMove, targetDirectory, targetProjectPath);
        return;
    }

    const std::string canonicalTargetDir = m_documentRegistry->canonicalize(targetDirectory);
    std::vector<std::string> createdPaths;
    for (const std::string& src : m_clipboardPaths) {
        auto st = m_services.fs.status(src);
        if (!st) {
            continue;
        }
        const bool isDir = st->type == io::FileType::Directory;
        const std::string dst = findNonCollidingCopyPath(m_services.fs, canonicalTargetDir, io::fileName(src), isDir);
        if (dst.empty()) {
            continue;
        }
        if (auto copied = copyFileOrDirectoryRecursiveFs(m_services.fs, src, dst); !copied) {
            notify(text("Cannot paste %1").arg(qs(io::fileName(src))), qs(copied.error().message()),
                   ui::NotificationKind::Error);
            continue;
        }
        createdPaths.push_back(m_documentRegistry->canonicalize(dst));
    }

    if (createdPaths.empty()) {
        return;
    }
    if (!targetProjectPath.empty() && targetProjectPath.ends_with(".vcxproj")) {
        if (auto added = addExistingFilesToVcxproj(m_services.fs, targetProjectPath, createdPaths, targetFilterPath);
            !added) {
            notify(text("Pasted files were not added to %1").arg(qs(io::fileName(targetProjectPath))),
                   qs(added.error().message()), ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(createdPaths.front());
        }
    }
    refreshSolutionExplorerForFsChange(createdPaths.front());
    if (m_solution) {
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
    }
}

void Workbench::duplicatePathInExplorer(const std::string& sourcePath, bool isDirectory, const std::string& projectPath,
                                        const std::string& filterPath) {
    if (sourcePath.empty()) {
        return;
    }
    const std::string canonicalSrc = m_documentRegistry->canonicalize(sourcePath);
    auto st = m_services.fs.status(canonicalSrc);
    if (!st) {
        notify(text("Cannot duplicate"), qs(st.error().message()), ui::NotificationKind::Error);
        return;
    }
    const std::string parentDir(io::parentPath(canonicalSrc));
    const std::string suggestedFull =
        findNonCollidingCopyPath(m_services.fs, parentDir, io::fileName(canonicalSrc), isDirectory);
    const std::string suggestedName =
        suggestedFull.empty() ? std::string(io::fileName(canonicalSrc)) : std::string(io::fileName(suggestedFull));

    std::string chosenName;
    if (m_window) {
        bool ok = false;
        const QString entered =
            QInputDialog::getText(m_window, isDirectory ? text("Duplicate Folder") : text("Duplicate File"),
                                  text("New name:"), QLineEdit::Normal, qs(suggestedName), &ok)
                .trimmed();
        if (!ok || entered.isEmpty()) {
            return;
        }
        chosenName = entered.toStdString();
    } else {
        chosenName = suggestedName;
    }

    if (auto valid = validateProjectNameComponent(chosenName); !valid) {
        notify(text("Cannot duplicate"), qs(valid.error().message()), ui::NotificationKind::Error);
        return;
    }
    auto dstJoined = io::joinPath(parentDir, chosenName);
    if (!dstJoined) {
        return;
    }
    if (m_services.fs.exists(*dstJoined).value_or(false)) {
        notify(text("Cannot duplicate"), text("'%1' already exists.").arg(qs(chosenName)), ui::NotificationKind::Error);
        return;
    }
    if (auto copied = copyFileOrDirectoryRecursiveFs(m_services.fs, canonicalSrc, *dstJoined); !copied) {
        notify(text("Cannot duplicate"), qs(copied.error().message()), ui::NotificationKind::Error);
        return;
    }
    const std::string canonicalDst = m_documentRegistry->canonicalize(*dstJoined);
    if (!projectPath.empty() && projectPath.ends_with(".vcxproj")) {
        const std::array<std::string, 1> duplicate{canonicalDst};
        if (auto added = addExistingFilesToVcxproj(m_services.fs, projectPath, duplicate, filterPath); !added) {
            notify(text("The copy was not added to %1").arg(qs(io::fileName(projectPath))), qs(added.error().message()),
                   ui::NotificationKind::Warning);
        }
    }
    if (m_nav) {
        m_nav->refresh();
        if (m_nav->fileSystemView()) {
            m_nav->fileSystemView()->selectWhenAvailable(canonicalDst);
        }
    }
    refreshSolutionExplorerForFsChange(canonicalDst);
    if (!isDirectory) {
        (void)openPath(canonicalDst);
    }
}

} // namespace frost::app
