// SPDX-License-Identifier: Apache-2.0
//
// Workbench file and folder creation, rename, and deletion operations and item dialogs.

#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "workbench_internal.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;
using workbench_detail::toUtf8;

namespace {

[[nodiscard]] std::string showNewItemDialog(QWidget* parent, const std::string& directory, bool folder) {
    QDialog dialog(parent);
    dialog.setObjectName(folder ? QStringLiteral("newFolderDialog") : QStringLiteral("newFileDialog"));
    dialog.setWindowTitle(folder ? text("New Folder") : text("New File"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* info = new QLabel(folder ? text("Create a new folder in %1:").arg(nativePath(directory))
                                   : text("Create a new file in %1:").arg(nativePath(directory)),
                            &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto* edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("newItemNameEdit"));
    edit->setPlaceholderText(folder ? text("Folder name") : text("File name (e.g. main.cpp)"));
    layout->addWidget(edit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return toUtf8(edit->text());
}

[[nodiscard]] std::string showRenameItemDialog(QWidget* parent, const std::string& currentPath, bool folder) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("renameItemDialog"));
    dialog.setWindowTitle(folder ? text("Rename Folder") : text("Rename File"));
    auto* layout = new QVBoxLayout(&dialog);
    const std::string_view currentName = nav::baseName(currentPath);
    auto* info = new QLabel(text("Rename '%1' to:").arg(fromUtf8(currentName)), &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto* edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("renameItemNameEdit"));
    const QString initial = fromUtf8(currentName);
    edit->setText(initial);
    if (!folder) {
        const qsizetype dot = initial.lastIndexOf(u'.');
        if (dot > 0) {
            edit->setSelection(0, static_cast<int>(dot));
        } else {
            edit->selectAll();
        }
    } else {
        edit->selectAll();
    }
    layout->addWidget(edit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    installDialogChrome(&dialog, false);
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return toUtf8(edit->text().trimmed());
}

[[nodiscard]] bool showDeleteConfirmationDialog(QWidget* parent, const std::string& path, bool folder) {
    const QString name = fromUtf8(nav::baseName(path));
    const QString prompt =
        folder ? text("Delete folder '%1' and all of its contents?").arg(name) : text("Delete file '%1'?").arg(name);
    return QMessageBox::question(parent, folder ? text("Delete Folder") : text("Delete File"), prompt,
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

} // namespace

void Workbench::notifyPathRenamed(const std::string& oldPath, const std::string& newPath) {
    if (oldPath == newPath) {
        return;
    }
    const auto remap = [&](const std::string& p) -> std::optional<std::string> {
        if (p == oldPath) {
            return newPath;
        }
        if (p.size() > oldPath.size() && isUnder(oldPath, p)) {
            const std::string rel = nav::relativeTo(oldPath, p);
            if (auto joined = io::joinPath(newPath, rel, nav::styleOf(newPath)); joined) {
                return m_services.documents.canonicalize(*joined);
            }
        }
        return std::nullopt;
    };

    std::vector<std::pair<std::string, std::string>> toRename;
    const auto collect = [&](const std::string& p) {
        if (auto mapped = remap(p)) {
            if (!std::ranges::any_of(toRename, [&](const auto& pair) { return pair.first == p; })) {
                toRename.emplace_back(p, std::move(*mapped));
            }
        }
    };
    collect(oldPath);
    for (const auto& [p, _] : m_lastPlaces) {
        collect(p);
    }
    for (const auto& [p, _] : m_bookmarks) {
        collect(p);
    }
    for (const std::string& p : m_recentFiles.paths()) {
        collect(p);
    }
    for (const nav::Place& pl : m_recentLocations.places()) {
        collect(pl.path);
    }
    for (const nav::Place& pl : m_history.backStack()) {
        collect(pl.path);
    }
    for (const nav::Place& pl : m_history.forwardStack()) {
        collect(pl.path);
    }

    for (const auto& [fromPath, toPath] : toRename) {
        if (const auto it = m_lastPlaces.find(fromPath); it != m_lastPlaces.end()) {
            nav::Place place = it->second;
            place.path = toPath;
            m_lastPlaces.erase(it);
            m_lastPlaces[toPath] = std::move(place);
        }
        if (const auto bmIt = m_bookmarks.find(fromPath); bmIt != m_bookmarks.end()) {
            std::set<int> lines = std::move(bmIt->second);
            m_bookmarks.erase(bmIt);
            m_bookmarks[toPath] = std::move(lines);
            syncEditorBookmarks(toPath);
        }
        for (auto& slot : m_numberedBookmarks) {
            if (slot && slot->path == fromPath) {
                slot->path = toPath;
            }
        }
        m_history.renamePath(fromPath, toPath);
        m_recentFiles.renamePath(fromPath, toPath);
        m_recentLocations.renamePath(fromPath, toPath);
    }

    if (auto mappedActive = remap(m_activePath)) {
        m_activePath = std::move(*mappedActive);
    }
    if (m_lastEditPlace) {
        if (auto mappedEdit = remap(m_lastEditPlace->path)) {
            m_lastEditPlace->path = std::move(*mappedEdit);
        }
    }
    if (!m_root.empty() && (isUnder(m_root, newPath) || isUnder(m_root, oldPath))) {
        if (m_fileSystem != nullptr && isUnder(m_root, newPath)) {
            m_fileSystem->selectWhenAvailable(newPath);
        }
        refresh();
    }
}

void Workbench::notifyPathDeleted(const std::string& deletedPath, bool directory) {
    const auto matches = [&](std::string_view p) { return p == deletedPath || (directory && isUnder(deletedPath, p)); };
    std::vector<std::string> affected;
    for (const auto& [path, _] : m_lastPlaces) {
        if (matches(path)) {
            affected.push_back(path);
        }
    }
    for (const std::string& p : m_recentFiles.paths()) {
        if (matches(p) && !std::ranges::contains(affected, p)) {
            affected.push_back(p);
        }
    }
    for (const nav::Place& pl : m_recentLocations.places()) {
        if (matches(pl.path) && !std::ranges::contains(affected, pl.path)) {
            affected.push_back(pl.path);
        }
    }
    for (const auto& [path, _] : m_bookmarks) {
        if (matches(path) && !std::ranges::contains(affected, path)) {
            affected.push_back(path);
        }
    }
    if (!std::ranges::contains(affected, deletedPath)) {
        affected.push_back(deletedPath);
    }
    for (const std::string& p : affected) {
        m_lastPlaces.erase(p);
        m_bookmarks.erase(p);
        for (auto& slot : m_numberedBookmarks) {
            if (slot && slot->path == p) {
                slot.reset();
            }
        }
        m_history.forget(p);
        m_recentFiles.remove(p);
        m_recentLocations.forget(p);
    }
    if (m_lastEditPlace && matches(m_lastEditPlace->path)) {
        m_lastEditPlace.reset();
    }
    if (!m_activePath.empty() && matches(m_activePath)) {
        const IEditorView* cur = m_shell.tabs().currentEditor();
        m_activePath = cur != nullptr ? cur->path() : std::string();
    }
    if (!m_root.empty() && (isUnder(m_root, deletedPath) || deletedPath == m_root)) {
        refresh();
    }
    m_shell.refreshActions();
}

std::string Workbench::defaultNewItemDirectory() const {
    if (m_services.defaultNewItemDirectory) {
        if (const std::string custom = m_services.defaultNewItemDirectory(); !custom.empty()) {
            return custom;
        }
    }
    if (m_fileSystem != nullptr) {
        if (const std::string selected = m_fileSystem->selectedDirectory(); !selected.empty()) {
            return selected;
        }
    }
    if (const IEditorView* editor = m_shell.tabs().currentEditor(); editor != nullptr && !editor->path().empty()) {
        const std::string parent(nav::parentOf(editor->path()));
        if (!parent.empty() && (m_root.empty() || isUnder(m_root, editor->path()) || parent == m_root)) {
            return parent;
        }
    }
    return m_root;
}

Result<std::string> Workbench::createNewFile(std::string_view directory, std::string_view relativeName) {
    const std::string targetDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    auto validated = validateNewItemTarget(m_services.fs, m_services.documents, targetDir, relativeName);
    if (!validated) {
        notifyError(text("Cannot create file"), validated.error());
        return std::unexpected(std::move(validated.error()));
    }
    const std::string target = std::move(*validated);
    if (m_services.saveManager.find(target) != nullptr) {
        Error err(ErrorCode::AlreadyExists, "'" + std::string(nav::baseName(target)) + "' is already open.");
        notifyError(text("Cannot create file"), err);
        return std::unexpected(std::move(err));
    }
    const std::string parent(nav::parentOf(target));
    if (!parent.empty()) {
        if (auto made = m_services.fs.createDirectories(parent); !made) {
            notifyError(text("Cannot create %1").arg(nativePath(target)), made.error());
            return std::unexpected(std::move(made.error()));
        }
    }
    if (auto exists = m_services.fs.exists(target); !exists || *exists) {
        Error err =
            !exists ? exists.error()
                    : Error(ErrorCode::AlreadyExists, "'" + std::string(nav::baseName(target)) + "' already exists.");
        notifyError(text("Cannot create %1").arg(nativePath(target)), err);
        return std::unexpected(std::move(err));
    }
    if (auto written = m_services.fs.writeFileAtomic(target, ""); !written) {
        notifyError(text("Cannot create %1").arg(nativePath(target)), written.error());
        return std::unexpected(std::move(written.error()));
    }
    const std::string canonical = m_services.documents.canonicalize(target);
    if (!m_root.empty() && isUnder(m_root, canonical)) {
        if (m_fileSystem != nullptr) {
            m_fileSystem->selectWhenAvailable(canonical);
        }
        refresh();
    }
    if (auto opened = openFile(canonical); !opened) {
        return std::unexpected(std::move(opened.error()));
    }
    if (m_services.fileCreated) {
        m_services.fileCreated(canonical);
    }
    return canonical;
}

Result<std::string> Workbench::createNewFolder(std::string_view directory, std::string_view relativeName) {
    const std::string targetDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    auto validated = validateNewItemTarget(m_services.fs, m_services.documents, targetDir, relativeName);
    if (!validated) {
        notifyError(text("Cannot create folder"), validated.error());
        return std::unexpected(std::move(validated.error()));
    }
    const std::string target = std::move(*validated);
    if (auto made = m_services.fs.createDirectories(target); !made) {
        notifyError(text("Cannot create %1").arg(nativePath(target)), made.error());
        return std::unexpected(std::move(made.error()));
    }
    const std::string canonical = m_services.documents.canonicalize(target);
    if (!m_root.empty() && isUnder(m_root, canonical)) {
        m_shell.toolWindows().show(kFileSystemToolWindow, false);
        if (m_fileSystem != nullptr) {
            m_fileSystem->selectWhenAvailable(canonical);
        }
        refresh();
    }
    return canonical;
}

void Workbench::promptAndCreateNewFile(std::string_view directory) {
    const std::string targetDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    if (targetDir.empty()) {
        notifyError(text("Cannot create file"), Error(ErrorCode::InvalidArgument, "Open a folder or project first."));
        return;
    }
    const std::string name = m_services.promptNewItem ? m_services.promptNewItem(targetDir, false)
                                                      : showNewItemDialog(&m_shell, targetDir, false);
    if (name.empty()) {
        return;
    }
    static_cast<void>(createNewFile(targetDir, name));
}

void Workbench::promptAndCreateNewFolder(std::string_view directory) {
    const std::string targetDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    if (targetDir.empty()) {
        notifyError(text("Cannot create folder"), Error(ErrorCode::InvalidArgument, "Open a folder or project first."));
        return;
    }
    const std::string name = m_services.promptNewItem ? m_services.promptNewItem(targetDir, true)
                                                      : showNewItemDialog(&m_shell, targetDir, true);
    if (name.empty()) {
        return;
    }
    static_cast<void>(createNewFolder(targetDir, name));
}

std::string Workbench::defaultTargetPathForModify() const {
    if (m_fileSystem != nullptr) {
        if (const std::string selected = m_fileSystem->selectedPath(); !selected.empty() && selected != m_root) {
            return selected;
        }
    }
    if (const IEditorView* editor = m_shell.tabs().currentEditor(); editor != nullptr && !editor->path().empty()) {
        return editor->path();
    }
    return {};
}

Result<std::string> Workbench::renamePath(std::string_view oldPath, std::string_view newRelativeName) {
    if (oldPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "No item is selected to rename.");
    }
    const std::string canonicalOld = m_services.documents.canonicalize(oldPath);
    if (!m_root.empty() && canonicalOld == m_root) {
        Error err(ErrorCode::InvalidArgument, "Cannot rename the project root folder.");
        notifyError(text("Cannot rename"), err);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonicalOld);
    if (!status) {
        notifyError(text("Cannot rename %1").arg(nativePath(canonicalOld)), status.error());
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (newRelativeName == nav::baseName(canonicalOld)) {
        return canonicalOld;
    }
    const std::string parentDir(nav::parentOf(canonicalOld));
    auto validated = validateNewItemTarget(m_services.fs, m_services.documents, parentDir, newRelativeName);
    if (!validated) {
        notifyError(text("Cannot rename %1").arg(nativePath(canonicalOld)), validated.error());
        return std::unexpected(std::move(validated.error()));
    }
    const std::string target = std::move(*validated);
    if (auto renamed = m_services.fs.rename(canonicalOld, target); !renamed) {
        notifyError(text("Cannot rename %1").arg(nativePath(canonicalOld)), renamed.error());
        return std::unexpected(std::move(renamed.error()));
    }
    const std::string canonicalNew = m_services.documents.canonicalize(target);
    if (!isDir) {
        if (document::FileDocument* doc = m_services.documents.find(canonicalOld); doc != nullptr) {
            (void)m_services.saveManager.renameDocument(*doc, canonicalNew);
            m_services.documents.renamePath(canonicalOld, canonicalNew);
            m_services.editors.renamePath(canonicalOld, canonicalNew);
            m_services.documents.notifyChanged(canonicalNew);
        }
        notifyPathRenamed(canonicalOld, canonicalNew);
    } else {
        std::vector<std::pair<std::string, std::string>> docRenames;
        for (const auto& doc : m_services.saveManager.documents()) {
            if (!doc->untitled() && isUnder(canonicalOld, doc->path())) {
                const std::string rel = nav::relativeTo(canonicalOld, doc->path());
                if (auto joined = io::joinPath(canonicalNew, rel, nav::styleOf(canonicalNew)); joined) {
                    docRenames.emplace_back(doc->path(), m_services.documents.canonicalize(*joined));
                }
            }
        }
        for (const auto& [oldDocPath, newDocPath] : docRenames) {
            if (document::FileDocument* doc = m_services.documents.find(oldDocPath); doc != nullptr) {
                (void)m_services.saveManager.renameDocument(*doc, newDocPath);
                m_services.documents.renamePath(oldDocPath, newDocPath);
                m_services.editors.renamePath(oldDocPath, newDocPath);
                m_services.documents.notifyChanged(newDocPath);
            }
        }
        notifyPathRenamed(canonicalOld, canonicalNew);
    }
    return canonicalNew;
}

Result<void> Workbench::deletePath(std::string_view path) {
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "No item is selected to delete.");
    }
    const std::string canonical = m_services.documents.canonicalize(path);
    if (!m_root.empty() && canonical == m_root) {
        Error err(ErrorCode::InvalidArgument, "Cannot delete the project root folder.");
        notifyError(text("Cannot delete"), err);
        return std::unexpected(std::move(err));
    }
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notifyError(text("Cannot delete %1").arg(nativePath(canonical)), status.error());
        return std::unexpected(std::move(status.error()));
    }
    const bool isDir = status->type == io::FileType::Directory;
    if (isDir) {
        if (auto removed = m_services.fs.removeAll(canonical); !removed) {
            notifyError(text("Cannot delete %1").arg(nativePath(canonical)), removed.error());
            return std::unexpected(std::move(removed.error()));
        }
    } else {
        if (auto removed = m_services.fs.remove(canonical); !removed) {
            notifyError(text("Cannot delete %1").arg(nativePath(canonical)), removed.error());
            return std::unexpected(std::move(removed.error()));
        }
    }
    std::vector<std::string> openToClose;
    for (EditorTabGroup* group : m_shell.tabs().groups()) {
        for (int i = 0; i < group->count(); ++i) {
            if (const IEditorView* ed = group->editor(i); ed != nullptr) {
                const std::string& p = ed->path();
                if ((p == canonical || (isDir && isUnder(canonical, p))) && !std::ranges::contains(openToClose, p)) {
                    openToClose.push_back(p);
                }
            }
        }
    }
    for (const auto& doc : m_services.saveManager.documents()) {
        if (!doc->untitled()) {
            const std::string& p = doc->path();
            if ((p == canonical || (isDir && isUnder(canonical, p))) && !std::ranges::contains(openToClose, p)) {
                openToClose.push_back(p);
            }
        }
    }
    for (const std::string& p : openToClose) {
        for (EditorTabGroup* group : m_shell.tabs().groups()) {
            for (;;) {
                const int idx = group->indexOf(p);
                if (idx < 0 || !group->close(idx)) {
                    break;
                }
            }
        }
        if (document::FileDocument* doc = m_services.documents.find(p); doc != nullptr) {
            (void)m_services.saveManager.close(*doc);
        }
    }
    notifyPathDeleted(canonical, isDir);
    return {};
}

void Workbench::promptAndRenamePath(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    if (m_services.renamePathHook) {
        m_services.renamePathHook(target);
        return;
    }
    const std::string canonical = m_services.documents.canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notifyError(text("Cannot rename %1").arg(nativePath(canonical)), status.error());
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    const std::string newName = m_services.promptRename ? m_services.promptRename(canonical, isDir)
                                                        : showRenameItemDialog(&m_shell, canonical, isDir);
    if (newName.empty() || newName == nav::baseName(canonical)) {
        return;
    }
    static_cast<void>(renamePath(canonical, newName));
}

void Workbench::promptAndDeletePath(std::string_view path) {
    const std::string target = path.empty() ? defaultTargetPathForModify() : std::string(path);
    if (target.empty()) {
        return;
    }
    if (m_services.deletePathHook) {
        m_services.deletePathHook(target);
        return;
    }
    const std::string canonical = m_services.documents.canonicalize(target);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        notifyError(text("Cannot delete %1").arg(nativePath(canonical)), status.error());
        return;
    }
    const bool isDir = status->type == io::FileType::Directory;
    const bool confirmed = m_services.confirmDelete ? m_services.confirmDelete(canonical, isDir)
                                                    : showDeleteConfirmationDialog(&m_shell, canonical, isDir);
    if (!confirmed) {
        return;
    }
    static_cast<void>(deletePath(canonical));
}

} // namespace frost::ui
