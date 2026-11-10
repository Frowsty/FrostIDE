// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): project source membership, adding, renaming and removing
// sources in CMakeLists.txt (with the edit preview) and in .vcxproj files, adding paths to the
// active project, the default target for a modification and the Solution Explorer refresh after a
// file system change (split from workbench_project.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_syntax.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "unity_control.hpp"
#include "workbench_project_helpers.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QInputDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <format>
#include <utility>

namespace frost::app {

namespace {

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

Result<std::string> cmakeRelativeSourcePath(std::string_view projectRoot, std::string_view sourcePath) {
    if (!sourcePath.starts_with(projectRoot) || sourcePath.size() <= projectRoot.size() ||
        (sourcePath[projectRoot.size()] != '/' && sourcePath[projectRoot.size()] != '\\')) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    std::string out(sourcePath.substr(projectRoot.size() + 1));
    std::ranges::replace(out, '\\', '/');
    while (out.starts_with('/')) {
        out.erase(out.begin());
    }
    if (out.empty() || out == "." || out == ".." || out.starts_with("../") || out.find("/../") != std::string::npos) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    for (const char raw : out) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch <= 0x20U || ch == '"' || ch == '#' || ch == '$' || ch == '(' || ch == ')' || ch == ';') {
            return fail(ErrorCode::Unsupported,
                        "the source path contains characters that require manual CMakeLists.txt quoting");
        }
    }
    return out;
}

/*
 * The changed lines of a CMakeLists.txt edit, for the preview the user confirms before anything is
 * written: the lines both texts share at the start and the end are left out, then "Line N:", the
 * removed lines prefixed "- " and the added lines prefixed "+ ". Pure; O(size).
 */
std::string cmakeEditPreview(std::string_view before, std::string_view after) {
    const auto lines = [](std::string_view text) {
        std::vector<std::string_view> out;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            out.push_back(line);
            start = end + 1;
        }
        return out;
    };
    const auto a = lines(before);
    const auto b = lines(after);
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    std::string preview = std::format("Line {}:\n", prefix + 1);
    for (std::size_t i = prefix; i < a.size() - suffix; ++i)
        preview += std::format("- {}\n", a[i]);
    for (std::size_t i = prefix; i < b.size() - suffix; ++i)
        preview += std::format("+ {}\n", b[i]);
    return preview;
}

} // namespace

bool DialogWorkbenchPrompts::confirmCMakeListsEdit(QWidget* parent, const std::string& listsPath,
                                                   const std::string& before, const std::string& after) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("cmakeEditPreviewDialog"));
    dialog.setWindowTitle(text("Edit CMakeLists.txt?"));
    dialog.setModal(true);
    dialog.resize(640, 360);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text("FrostIDE would change %1 as shown. Nothing is written unless you apply it.")
                                 .arg(QDir::toNativeSeparators(qs(listsPath))),
                             &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* preview = new QPlainTextEdit(qs(cmakeEditPreview(before, after)), &dialog);
    preview->setObjectName(QStringLiteral("cmakeEditPreviewText"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(preview, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    ui::installDialogChrome(&dialog, false);
    return dialog.exec() == QDialog::Accepted;
}

Result<void> Workbench::addSourceToCMakeLists(std::string_view sourcePath, std::string_view targetName) {
    if (m_projectRoot.empty()) {
        return fail(ErrorCode::InvalidArgument, "no project is open");
    }
    Result<std::string> listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return std::unexpected(std::move(listsPath.error()));
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return fail(ErrorCode::NotFound, "CMakeLists.txt was not found in the project root");
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        Error err(ErrorCode::Busy, "save or revert unsaved changes in CMakeLists.txt before adding a source file");
        notify(text("Cannot update CMakeLists.txt"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const std::string canonicalSource = m_documentRegistry->canonicalize(sourcePath);
    if (!m_services.fs.exists(canonicalSource).value_or(false)) {
        return fail(ErrorCode::NotFound, "source file does not exist: " + canonicalSource);
    }
    Result<std::string> relSource = cmakeRelativeSourcePath(canonicalRoot, canonicalSource);
    if (!relSource) {
        notify(text("Manual CMakeLists.txt edit required"), qs(relSource.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(relSource.error()));
    }

    Result<std::string> content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        notify(text("Cannot read CMakeLists.txt"), qs(content.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(content.error()));
    }

    Result<std::vector<project::CMakeSyntaxCommand>> commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        notify(text("Manual CMakeLists.txt edit required"), qs(commands.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(commands.error()));
    }

    std::vector<const project::CMakeSyntaxCommand*> candidates;
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.scopeDepth != 0 || cmd.nestedArguments || cmd.name == "add_subdirectory" || cmd.name == "include") {
            Error err(ErrorCode::Unsupported,
                      "CMakeLists.txt uses conditional or multi-directory commands; update it manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        if (cmd.name != "add_executable" && cmd.name != "add_library") {
            continue;
        }
        if (cmd.arguments.empty() || !std::ranges::all_of(cmd.arguments, &project::CMakeSyntaxArgument::isLiteral)) {
            Error err(ErrorCode::Unsupported,
                      "CMake target declaration uses non-literal arguments; update CMakeLists.txt manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        const bool isImportedOrAlias = std::ranges::any_of(cmd.arguments, [](const project::CMakeSyntaxArgument& arg) {
            return arg.literal == "IMPORTED" || arg.literal == "ALIAS" || arg.literal == "INTERFACE";
        });
        if (isImportedOrAlias) {
            continue;
        }
        if (!targetName.empty() && cmd.arguments.front().literal != targetName) {
            continue;
        }
        candidates.push_back(&cmd);
    }

    if (candidates.size() != 1) {
        Error err(ErrorCode::Unsupported, "CMakeLists.txt does not have a single simple top-level C/C++ target");
        notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const project::CMakeSyntaxCommand* selected = candidates.front();
    for (std::size_t i = 1; i < selected->arguments.size(); ++i) {
        if (selected->arguments[i].literal == *relSource) {
            return {};
        }
    }

    const std::string_view newline = content->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t firstArgBegin = selected->arguments.front().begin;
    const std::size_t closePos = selected->close;
    const std::string_view spanView = std::string_view(*content).substr(firstArgBegin, closePos - firstArgBegin);
    const bool multiline = spanView.find('\n') != std::string_view::npos;

    std::string updated = *content;
    if (multiline) {
        std::size_t lineStart = closePos;
        while (lineStart > 0 && (updated[lineStart - 1] == ' ' || updated[lineStart - 1] == '\t')) {
            --lineStart;
        }
        if (lineStart > 0 && updated[lineStart - 1] == '\n') {
            updated.insert(lineStart, std::format("    {}{}", *relSource, newline));
        } else {
            updated.insert(closePos, std::format("{}    {}{}", newline, *relSource, newline));
        }
    } else {
        updated.insert(closePos, std::format(" {}", *relSource));
    }

    if (Result<std::vector<project::CMakeSyntaxCommand>> verified = project::scanCMakeSyntax(updated); !verified) {
        return std::unexpected(std::move(verified.error()));
    }
    if (auto written = writeCMakeListsWithPreview(
            canonicalLists, *content, updated,
            text("Adding %1 to target %2").arg(qs(*relSource), qs(selected->arguments.front().literal)));
        !written) {
        return written;
    }
    notify(text("CMakeLists.txt updated"),
           text("Added %1 to target %2.").arg(qs(*relSource), qs(selected->arguments.front().literal)),
           ui::NotificationKind::Info);
    return {};
}

/*
 * CMakeLists.txt is edited as text (add, rename and remove a source); FrostIDE writes such an edit
 * only after the user saw it: the prompt shows the changed lines (cmakeEditPreview) and nothing is
 * written unless the user applies them. Declined, the file stays byte for byte as it was and an
 * Info notification says so (Cancelled). The write is atomic (safeWrite); an open CMakeLists.txt
 * tab reloads from disk.
 */
Result<void> Workbench::writeCMakeListsWithPreview(const std::string& canonicalLists, const std::string& before,
                                                   const std::string& after, const QString& change) {
    if (!m_services.prompts.confirmCMakeListsEdit(m_window, canonicalLists, before, after)) {
        notify(text("CMakeLists.txt not changed"),
               text("%1 was not applied to CMakeLists.txt; update it manually if the target needs it.").arg(change),
               ui::NotificationKind::Info);
        return fail(ErrorCode::Cancelled, "the CMakeLists.txt edit was declined");
    }
    if (Result<io::SafeWriteResult> written = m_services.fs.safeWrite(canonicalLists, after, {}); !written) {
        notify(text("Cannot update CMakeLists.txt"), qs(written.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(written.error()));
    }
    if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
        (void)m_documents->checkDisk(*openLists);
        m_documentRegistry->notifyChanged(canonicalLists);
    }
    return {};
}

std::string Workbench::defaultTargetPathForModify() const {
    const bool filesVisible = m_window && m_window->toolWindows().isVisible(ui::kFileSystemToolWindow);
    const bool projectVisible = m_window && m_window->toolWindows().isVisible(ui::tool_windows::kProject);
    if (m_solution && m_explorer && (projectVisible || !filesVisible)) {
        if (const auto nodeId = m_explorer->currentNode()) {
            const auto& node = m_explorer->solution()->node(*nodeId); // The shown model's ids.
            if ((node.kind == project::NodeKind::File || node.kind == project::NodeKind::Filter) &&
                !node.path.empty()) {
                return node.path;
            }
        }
    }
    if (m_nav && m_nav->fileSystemView()) {
        const std::string selected = m_nav->fileSystemView()->selectedPath();
        if (!selected.empty() && selected != m_projectRoot) {
            return selected;
        }
    }
    if (m_solution && m_explorer) {
        if (const auto nodeId = m_explorer->currentNode()) {
            const auto& node = m_explorer->solution()->node(*nodeId); // The shown model's ids.
            if ((node.kind == project::NodeKind::File || node.kind == project::NodeKind::Filter) &&
                !node.path.empty()) {
                return node.path;
            }
        }
    }
    if (const DocumentEditorView* active = currentView(); active != nullptr && !active->path().empty()) {
        return active->path();
    }
    return {};
}

void Workbench::refreshSolutionExplorerForFsChange(std::string_view selectPath) {
    if (!m_solution || m_buildService->state() != build::BuildState::Idle || m_buildService->projectEditInProgress()) {
        return;
    }
    if (auto loaded = project::SolutionModel::load(
            m_services.fs, m_solution->path(),
            {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths})) {
        auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
        if (m_buildService->beginProjectEdit()) {
            (void)m_buildService->finishProjectEdit(refreshed);
        }
        m_solution = refreshed;
        if (m_build) {
            m_build->projectEditPublished();
        }
        if (m_runService) {
            (void)m_runService->openSolution(refreshed);
        }
        if (m_explorer) {
            m_explorer->setSolution(refreshed, true);
            if (!selectPath.empty()) {
                (void)m_explorer->selectPath(selectPath);
            }
        }
        if (m_nav) {
            (void)m_nav->refreshSolution(*refreshed);
        }
    }
}

void Workbench::addPathsToActiveProject(const std::vector<std::string>& paths, std::string_view targetFilterPath) {
    if (paths.empty()) {
        return;
    }
    if (m_solution) {
        std::vector<const project::ProjectInfo*> cppProjects;
        const project::ProjectInfo* bestMatch = nullptr;
        std::size_t bestMatchLen = 0;
        const std::string firstKey = lang::documentKey(paths.front());

        for (const project::ProjectInfo& proj : m_solution->projects()) {
            if (proj.kind == project::ProjectKind::Cpp && proj.nodeKind == project::NodeKind::Project &&
                !proj.path.empty()) {
                cppProjects.push_back(&proj);
                const std::string projDirKey = lang::documentKey(io::parentPath(proj.path));
                if (firstKey.starts_with(projDirKey) && projDirKey.size() > bestMatchLen) {
                    bestMatch = &proj;
                    bestMatchLen = projDirKey.size();
                }
            }
        }
        if (cppProjects.empty()) {
            notify(text("Add to Project"),
                   text("No loaded C++ (.vcxproj) project is open in the solution. C# SDK-style projects include files "
                        "in their directory automatically."),
                   ui::NotificationKind::Info);
            return;
        }
        // Regular files go to the project in one ProjectHost edit (SLN-6), filed under the drop's
        // filter; a folder is refused before the user is asked anything.
        const bool regularFiles = std::ranges::all_of(paths, [&](const std::string& path) {
            const auto status = m_services.fs.status(path);
            return status && status->type == io::FileType::Regular;
        });
        if (!regularFiles) {
            notify(text("Add to Project"),
                   text("Adding a folder to a project is not supported; add its files instead. No file was changed."),
                   ui::NotificationKind::Error);
            return;
        }
        std::string targetVcxproj;
        std::string targetProjName;
        if (bestMatch != nullptr) {
            targetVcxproj = bestMatch->path;
            targetProjName = bestMatch->name;
        } else if (cppProjects.size() == 1) {
            targetVcxproj = cppProjects.front()->path;
            targetProjName = cppProjects.front()->name;
        } else if (m_window) {
            QStringList names;
            for (const project::ProjectInfo* p : cppProjects) {
                names.push_back(qs(p->name));
            }
            bool ok = false;
            const QString chosen = QInputDialog::getItem(m_window, text("Add to Project"),
                                                         text("Select target C++ project:"), names, 0, false, &ok);
            if (!ok || chosen.isEmpty()) {
                return;
            }
            const qsizetype idx = names.indexOf(chosen);
            if (idx >= 0 && static_cast<std::size_t>(idx) < cppProjects.size()) {
                targetVcxproj = cppProjects[static_cast<std::size_t>(idx)]->path;
                targetProjName = cppProjects[static_cast<std::size_t>(idx)]->name;
            }
        }
        if (targetVcxproj.empty()) {
            return;
        }
        addProjectFiles(qs(targetVcxproj), paths, qs(targetFilterPath));
        return;
    }

    if (!m_projectRoot.empty()) {
        const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
        if (listsPath && m_services.fs.exists(*listsPath).value_or(false)) {
            std::vector<std::string> sourceFiles;
            const auto collectSources = [&](const auto& self, const std::string& p) -> void {
                auto st = m_services.fs.status(p);
                if (!st) {
                    return;
                }
                if (st->type == io::FileType::Regular) {
                    if (isCFamilySource(p)) {
                        sourceFiles.push_back(p);
                    }
                } else if (st->type == io::FileType::Directory) {
                    if (auto entries = m_services.fs.listDirectory(p)) {
                        for (const io::DirEntry& e : *entries) {
                            if (e.name == ".git" || e.name == "build" || e.name == "out") {
                                continue;
                            }
                            if (auto child = io::joinPath(p, e.name)) {
                                self(self, *child);
                            }
                        }
                    }
                }
            };
            for (const std::string& p : paths) {
                collectSources(collectSources, p);
            }
            for (const std::string& src : sourceFiles) {
                (void)addSourceToCMakeLists(src);
            }
            return;
        }
    }
    notify(text("Add to Project"), text("Open a C++ solution (.sln / .slnx) or CMake project first."),
           ui::NotificationKind::Warning);
}

void Workbench::renameSourceInCMakeLists(const std::string& oldPath, const std::string& newPath, bool directory) {
    if (m_projectRoot.empty()) {
        return;
    }
    const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return;
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return;
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        return;
    }
    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const auto relOld = cmakeRelativeSourcePath(canonicalRoot, oldPath);
    const auto relNew = cmakeRelativeSourcePath(canonicalRoot, newPath);
    if (!relOld || !relNew) {
        return;
    }
    const auto content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        return;
    }
    const auto commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        return;
    }
    struct Replacement {
        std::size_t begin;
        std::size_t end;
        std::string text;
    };
    std::vector<Replacement> edits;
    const std::string oldPrefix = *relOld + "/";
    const std::string newPrefix = *relNew + "/";
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.name != "add_executable" && cmd.name != "add_library" && cmd.name != "target_sources") {
            continue;
        }
        for (std::size_t i = 1; i < cmd.arguments.size(); ++i) {
            const auto& arg = cmd.arguments[i];
            if (!arg.isLiteral) {
                continue;
            }
            if (!directory && arg.literal == *relOld) {
                const bool quoted = (*content)[arg.begin] == '"';
                edits.push_back(Replacement{arg.begin, arg.end, quoted ? "\"" + *relNew + "\"" : *relNew});
            } else if (directory && arg.literal.starts_with(oldPrefix)) {
                const std::string replaced = newPrefix + arg.literal.substr(oldPrefix.size());
                const bool quoted = (*content)[arg.begin] == '"';
                edits.push_back(Replacement{arg.begin, arg.end, quoted ? "\"" + replaced + "\"" : replaced});
            }
        }
    }
    if (edits.empty()) {
        return;
    }
    std::string updated = *content;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        updated.replace(it->begin, it->end - it->begin, it->text);
    }
    if (!project::scanCMakeSyntax(updated)) {
        return;
    }
    (void)writeCMakeListsWithPreview(canonicalLists, *content, updated,
                                     text("Renaming %1 to %2").arg(qs(*relOld), qs(*relNew)));
}

void Workbench::removeSourceFromCMakeLists(const std::string& deletedPath, bool directory) {
    if (m_projectRoot.empty()) {
        return;
    }
    const auto listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return;
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return;
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        return;
    }
    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const auto relDeleted = cmakeRelativeSourcePath(canonicalRoot, deletedPath);
    if (!relDeleted) {
        return;
    }
    const auto content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        return;
    }
    const auto commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        return;
    }
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const std::string prefix = *relDeleted + "/";
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.name != "add_executable" && cmd.name != "add_library" && cmd.name != "target_sources") {
            continue;
        }
        for (std::size_t i = 1; i < cmd.arguments.size(); ++i) {
            const auto& arg = cmd.arguments[i];
            if (!arg.isLiteral) {
                continue;
            }
            if ((!directory && arg.literal == *relDeleted) || (directory && arg.literal.starts_with(prefix))) {
                std::size_t b = arg.begin;
                std::size_t e = arg.end;
                while (b > 0 && ((*content)[b - 1] == ' ' || (*content)[b - 1] == '\t')) {
                    --b;
                }
                if (b > 0 && (*content)[b - 1] == '\n' && e < content->size() &&
                    ((*content)[e] == '\n' ||
                     ((*content)[e] == '\r' && e + 1 < content->size() && (*content)[e + 1] == '\n'))) {
                    e += ((*content)[e] == '\r') ? 2 : 1;
                }
                spans.emplace_back(b, e);
            }
        }
    }
    if (spans.empty()) {
        return;
    }
    std::string updated = *content;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        updated.erase(it->first, it->second - it->first);
    }
    if (!project::scanCMakeSyntax(updated)) {
        return;
    }
    (void)writeCMakeListsWithPreview(canonicalLists, *content, updated, text("Removing %1").arg(qs(*relDeleted)));
}

/*
 * Keeps the loaded projects in step with a rename, move or delete that has already happened on
 * disk. A C++ project that lists `oldPath` (or, for a folder, files under it) as items written
 * without a wildcard gets them renamed or removed in its .vcxproj and .vcxproj.filters through
 * ProjectHost (SLN-6, ADR 0082; one queued edit per project, asynchronous). When such an edit
 * cannot start, and for a C# project whose Include attribute names the path (.csproj files are not
 * edited), a warning names the projects that still list the old path. Reads each listed .csproj
 * once; O(project items).
 */
void Workbench::updatePathInVcxproj(const std::string& oldPath, const std::string& newPath, bool directory,
                                    bool deleted) {
    if (!m_solution || oldPath.empty()) {
        return;
    }
    const std::string oldKey = lang::documentKey(oldPath);
    const QString name = qs(io::fileName(oldPath));
    const QString what =
        deleted ? text("%1 was deleted").arg(name)
                : text("%1 was %2 to %3")
                      .arg(name, io::parentPath(oldPath) == io::parentPath(newPath) ? text("renamed") : text("moved"),
                           QDir::toNativeSeparators(qs(newPath)));
    QStringList stale;
    QString reason;
    for (project::ProjectPathChange& change :
         project::projectEditsForPathChange(*m_solution, oldPath, deleted ? std::string_view{} : newPath, directory)) {
        project::ProjectEdit edit;
        edit.kind = deleted ? project::ProjectEditKind::RemoveFiles : project::ProjectEditKind::RenameFiles;
        edit.definition = change.project;
        edit.files = std::move(change.files);
        edit.renamedTo = std::move(change.renamedTo);
        if (auto refused = submitProjectEdit(std::move(edit), text("Update project files"))) {
            stale.push_back(qs(io::fileName(change.project)));
            reason = *refused;
        }
    }
    if (!stale.isEmpty()) {
        notify(text("Project files not updated"),
               text("%1, but %2 still list its old path: %3 No project file was changed. Update the project "
                    "manually.")
                   .arg(what, stale.join(QStringLiteral(", ")), reason),
               ui::NotificationKind::Warning);
        stale.clear();
    }
    for (const project::ProjectInfo& proj : m_solution->projects()) {
        if (proj.nodeKind != project::NodeKind::Project || proj.path.empty()) {
            continue;
        }
        bool listed = false;
        if (proj.kind == project::ProjectKind::CSharp) {
            // SDK-style projects include their folder's files implicitly; only an explicit Include needs an edit.
            const std::string projDir(io::parentPath(proj.path));
            const std::string projKey = lang::documentKey(projDir);
            if (oldKey.size() > projKey.size() && oldKey.starts_with(projKey)) {
                std::string rel = oldPath.substr(projDir.size() + 1);
                std::ranges::replace(rel, '/', '\\');
                if (auto content = m_services.fs.readFile(proj.path)) {
                    std::string forward = rel;
                    std::ranges::replace(forward, '\\', '/');
                    for (const std::string& spelling : {rel, forward}) {
                        const std::string attribute = "Include=\"" + escapeXml(spelling);
                        const std::string exact = attribute + (directory ? (spelling == rel ? "\\" : "/") : "\"");
                        if (content->find(exact) != std::string::npos)
                            listed = true;
                    }
                }
            }
        }
        if (listed)
            stale.push_back(qs(io::fileName(proj.path)));
    }
    if (stale.isEmpty()) {
        return;
    }
    notify(text("Project files not updated"),
           text("%1, but %2 still list its old path. FrostIDE does not edit C# project files; no project file was "
                "changed. Update the project manually.")
               .arg(what, stale.join(QStringLiteral(", "))),
           ui::NotificationKind::Warning);
}

} // namespace frost::app
