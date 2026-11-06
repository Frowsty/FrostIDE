// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/ui/workbench/workbench.hpp): actions, popups, project and session handling, hot
// exit, external changes, saving and autosave over the ShellWindow.

#include "frost/ui/workbench/workbench.hpp"

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/search/fuzzy_path.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/nav/paths.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "workbench_util.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>

namespace frost::ui {

using workbench_detail::fileIcon;
using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;
using workbench_detail::themedIcon;
using workbench_detail::toUtf8;

namespace {

// Actions run from the main window, not from popups or dialogs (as the shell's own).
constexpr input::ContextRule kWindowRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};
// The i18n context of action names from the registry (the shell's).
constexpr std::string_view kActionsContext = "frost::input::actions";

[[nodiscard]] QString lowerExtension(std::string_view path) {
    const std::string_view name = nav::baseName(path);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? QString() : fromUtf8(name.substr(dot)).toLower();
}

[[nodiscard]] bool isUnder(std::string_view root, std::string_view path) {
    if (root.empty() || path.size() <= root.size() || !path.starts_with(root)) {
        return false;
    }
    const char next = path[root.size()];
    return next == '/' || next == '\\' || root.back() == '/' || root.back() == '\\';
}

[[nodiscard]] bool isReservedWindowsDeviceStem(std::string_view stem) noexcept {
    std::string upper(stem);
    std::ranges::transform(upper, upper.begin(),
                           [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' &&
        upper[3] <= '9') {
        return true;
    }
    return false;
}

[[nodiscard]] Result<std::string> validateNewItemTarget(io::IFileSystem& fs, const nav::DocumentRegistry& documents,
                                                        std::string_view directory, std::string_view relativeName) {
    if (directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "No folder is open to create an item in.");
    }
    if (relativeName.empty()) {
        return fail(ErrorCode::InvalidArgument, "The name must not be empty.");
    }
    if (relativeName.front() == ' ' || relativeName.back() == ' ' || relativeName.front() == '/' ||
        relativeName.front() == '\\' || io::isAbsolutePath(relativeName, io::PathStyle::Posix) ||
        io::isAbsolutePath(relativeName, io::PathStyle::Windows)) {
        return fail(ErrorCode::InvalidArgument, "Enter a relative name inside the target folder.");
    }
    std::size_t pos = 0;
    while (pos <= relativeName.size()) {
        const std::size_t sep = relativeName.find_first_of("/\\", pos);
        const std::string_view segment =
            sep == std::string_view::npos ? relativeName.substr(pos) : relativeName.substr(pos, sep - pos);
        if (segment.empty() || segment == "." || segment == "..") {
            return fail(ErrorCode::InvalidArgument, "The path may not contain empty, '.' or '..' segments.");
        }
        if (segment.back() == '.' || segment.back() == ' ') {
            return fail(ErrorCode::InvalidArgument, "A file or folder name may not end with a dot or space.");
        }
        for (const char ch : segment) {
            const auto uc = static_cast<unsigned char>(ch);
            if (uc < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
                return fail(ErrorCode::InvalidArgument,
                            "The name contains characters that are not allowed in file names.");
            }
        }
        const std::size_t dot = segment.find('.');
        const std::string_view stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);
        if (isReservedWindowsDeviceStem(stem)) {
            return fail(ErrorCode::InvalidArgument, "The name is a reserved device name.");
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1;
    }
    const std::string baseDir = documents.canonicalize(directory);
    auto baseStatus = fs.status(baseDir);
    if (!baseStatus) {
        return std::unexpected(std::move(baseStatus.error()));
    }
    if (baseStatus->type != io::FileType::Directory) {
        return fail(ErrorCode::InvalidArgument, "'" + baseDir + "' is not a directory.");
    }
    const io::PathStyle style = nav::styleOf(baseDir);
    auto joined = io::joinPath(baseDir, relativeName, style);
    if (!joined) {
        return std::unexpected(std::move(joined.error()));
    }
    auto normalized = io::normalizePath(*joined, style);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (!isUnder(baseDir, *normalized)) {
        return fail(ErrorCode::InvalidArgument, "The target path must stay inside the selected folder.");
    }
    auto exists = fs.exists(*normalized);
    if (!exists) {
        return std::unexpected(std::move(exists.error()));
    }
    if (*exists) {
        return fail(ErrorCode::AlreadyExists, "'" + std::string(nav::baseName(*normalized)) + "' already exists.");
    }
    return *normalized;
}

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

[[nodiscard]] bool isTodoScannablePath(std::string_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    const auto toLowerAscii = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    };
    const auto containsSegmentInsensitive = [&](std::string_view seg) noexcept {
        for (std::size_t i = 0; i + seg.size() + 2 <= path.size(); ++i) {
            const char prev = path[i];
            if (prev != '/' && prev != '\\') {
                continue;
            }
            const char next = path[i + 1 + seg.size()];
            if (next != '/' && next != '\\') {
                continue;
            }
            bool match = true;
            for (std::size_t j = 0; j < seg.size(); ++j) {
                if (toLowerAscii(path[i + 1 + j]) != seg[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    };

    static constexpr std::string_view kIgnoredDirs[] = {
        "library", "temp", "logs", "usersettings", "memorycaptures", "packagecache",
        "obj",     "bin",  "out",  "build",        "node_modules",   ".git",
        ".vs",     ".svn", ".hg",  "artifacts",    "shadercache",    "il2cpp_cache"};
    for (std::string_view seg : kIgnoredDirs) {
        if (containsSegmentInsensitive(seg)) {
            return false;
        }
    }

    const std::string_view name = nav::baseName(path);
    if (name == "CMakeLists.txt" || name == "Makefile" || name == "Dockerfile") {
        return true;
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) {
        return false;
    }
    const std::string_view rawExt = name.substr(dot);
    if (rawExt.size() > 12) {
        return false;
    }
    char extBuf[12]{};
    for (std::size_t i = 0; i < rawExt.size(); ++i) {
        extBuf[i] = toLowerAscii(rawExt[i]);
    }
    const std::string_view ext(extBuf, rawExt.size());

    static constexpr std::string_view kSourceExts[] = {
        ".c",      ".cc",     ".cpp",     ".cxx",   ".c++",  ".cppm", ".ixx",  ".h",    ".hh",   ".hpp",
        ".hxx",    ".inl",    ".ipp",     ".cs",    ".csx",  ".fs",   ".vb",   ".lua",  ".luau", ".hlsl",
        ".hlsli",  ".glsl",   ".vert",    ".frag",  ".comp", ".shader", ".cginc", ".compute", ".uss", ".uxml",
        ".py",     ".js",     ".ts",      ".jsx",   ".tsx",  ".rs",   ".go",   ".java", ".kt",   ".swift",
        ".cmake",  ".txt",    ".md",      ".sh",    ".ps1",  ".bat",  ".cmd"};
    for (std::string_view allowed : kSourceExts) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasCommentPrefixBefore(std::string_view line, std::size_t pos) noexcept {
    const std::string_view prefix = line.substr(0, pos);
    if (prefix.find("//") != std::string_view::npos || prefix.find("/*") != std::string_view::npos ||
        prefix.find("--") != std::string_view::npos || prefix.find("<!--") != std::string_view::npos ||
        prefix.find('#') != std::string_view::npos) {
        return true;
    }
    const std::size_t firstNonSpace = prefix.find_first_not_of(" \t");
    return firstNonSpace != std::string_view::npos && (prefix[firstNonSpace] == '*' || prefix[firstNonSpace] == ';');
}

[[nodiscard]] bool mayContainTodoTag(std::string_view content) noexcept {
    const auto toUpper = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    };
    for (std::size_t i = 0; i + 3 <= content.size(); ++i) {
        const char c0 = toUpper(content[i]);
        if (c0 == 'T' && i + 4 <= content.size() && toUpper(content[i + 1]) == 'O' &&
            toUpper(content[i + 2]) == 'D' && toUpper(content[i + 3]) == 'O') {
            return true;
        }
        if (c0 == 'F' && i + 5 <= content.size() && toUpper(content[i + 1]) == 'I' &&
            toUpper(content[i + 2]) == 'X' && toUpper(content[i + 3]) == 'M' && toUpper(content[i + 4]) == 'E') {
            return true;
        }
        if (c0 == 'H' && i + 4 <= content.size() && toUpper(content[i + 1]) == 'A' &&
            toUpper(content[i + 2]) == 'C' && toUpper(content[i + 3]) == 'K') {
            return true;
        }
        if (c0 == 'B' && toUpper(content[i + 1]) == 'U' && toUpper(content[i + 2]) == 'G') {
            return true;
        }
        if (c0 == 'U' && i + 6 <= content.size() && toUpper(content[i + 1]) == 'N' &&
            toUpper(content[i + 2]) == 'D' && toUpper(content[i + 3]) == 'O' && toUpper(content[i + 4]) == 'N' &&
            toUpper(content[i + 5]) == 'E') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::pair<std::size_t, std::string_view>> findTodoTag(std::string_view line) {
    static constexpr std::string_view kTags[] = {"TODO", "FIXME", "HACK", "UNDONE", "BUG"};
    const auto isWordChar = [](char c) noexcept {
        const auto uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '_';
    };
    const auto toUpperAscii = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    };
    const auto matchesTagAt = [&](std::size_t pos, std::string_view tag, bool& exactUpper) noexcept {
        if (pos + tag.size() > line.size()) {
            return false;
        }
        exactUpper = true;
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (line[pos + i] != tag[i]) {
                exactUpper = false;
            }
            if (toUpperAscii(line[pos + i]) != tag[i]) {
                return false;
            }
        }
        return true;
    };
    std::optional<std::pair<std::size_t, std::string_view>> best;
    for (std::string_view tag : kTags) {
        for (std::size_t pos = 0; pos + tag.size() <= line.size(); ++pos) {
            bool exactUpper = false;
            if (!matchesTagAt(pos, tag, exactUpper)) {
                continue;
            }
            const bool leftOk = (pos == 0) || !isWordChar(line[pos - 1]);
            const std::size_t end = pos + tag.size();
            const bool rightOk = (end >= line.size()) || !isWordChar(line[end]);
            if (!leftOk || !rightOk) {
                continue;
            }
            const bool inComment = hasCommentPrefixBefore(line, pos);
            const bool hasColon = (end < line.size() && line[end] == ':');
            const bool isPrimaryTag = (tag == "TODO" || tag == "FIXME");
            if (!inComment && !hasColon && !(isPrimaryTag && exactUpper)) {
                continue;
            }
            if (!best || pos < best->first) {
                best = std::pair<std::size_t, std::string_view>{pos, tag};
            }
            break;
        }
    }
    return best;
}

[[nodiscard]] QVariant fileData(const std::string& path, std::optional<int> line = std::nullopt,
                                std::optional<int> column = std::nullopt) {
    QVariantMap map;
    map.insert(QStringLiteral("kind"), QStringLiteral("file"));
    map.insert(QStringLiteral("path"), fromUtf8(path));
    if (line) {
        map.insert(QStringLiteral("line"), *line);
        map.insert(QStringLiteral("column"), column.value_or(1));
    }
    return map;
}

} // namespace

/*
 * The watcher pump: a task on the scheduler awaits the watch's batches and posts each to the
 * workbench's thread. The mailbox's target is cleared (under its mutex) before the workbench goes,
 * so no batch is posted afterwards; batches already queued die with the workbench (their context
 * object).
 */
struct WatchMailbox {
    std::mutex mutex;
    Workbench* target = nullptr;

    void post(io::FileChangeBatch batch) {
        const std::scoped_lock lock(mutex);
        if (target != nullptr) {
            QMetaObject::invokeMethod(
                target, [target = target, batch = std::move(batch)] { target->applyFileChanges(batch); },
                Qt::QueuedConnection);
        }
    }
};

namespace {

// The scheduler owns the final reference so large old trees are freed away from the GUI.
sched::Task<void> retireProjectFiles(std::shared_ptr<const nav::ProjectFiles> files) {
    files.reset();
    co_return;
}

void retireSnapshot(sched::IScheduler* scheduler, std::shared_ptr<const nav::ProjectFiles> files) {
    if (scheduler && files) {
        scheduler->spawn(sched::Lane::Background, retireProjectFiles(std::move(files))).detach();
    }
}

sched::Task<void> retireSolutionFiles(std::shared_ptr<const std::vector<std::string>> files) {
    files.reset();
    co_return;
}

void retireSolutionSnapshot(sched::IScheduler* scheduler, std::shared_ptr<const std::vector<std::string>> files) {
    if (scheduler && files) {
        scheduler->spawn(sched::Lane::Background, retireSolutionFiles(std::move(files))).detach();
    }
}

sched::Task<nav::ProjectFiles> prepareInitialFiles(std::string root,
                                                   std::shared_ptr<const std::vector<std::string>> solutionFiles,
                                                   std::stop_token token) {
    nav::ProjectFiles files(std::move(root));
    if (token.stop_requested() || !files.setSolutionFilesCancellable(*solutionFiles, token))
        files = nav::ProjectFiles();
    solutionFiles.reset();
    co_return files;
}

// Enumeration and all path/index preparation run on the background lane.
sched::Task<Result<std::shared_ptr<const nav::ProjectFiles>>>
prepareProjectFiles(io::IFileSystem& fs, sched::IScheduler& scheduler, std::string root,
                    std::shared_ptr<const std::vector<std::string>> solutionFiles,
                    std::shared_ptr<const nav::ProjectFiles> previous, std::stop_token token) {
    // Locals die on this worker; coroutine parameters survive until the GUI joins the frame.
    auto baseline = std::move(previous);
    nav::ProjectFiles files(root);
    if (!files.setSolutionFilesCancellable(*solutionFiles, token)) {
        files = nav::ProjectFiles();
        solutionFiles.reset();
        co_return fail(ErrorCode::Cancelled, "project scan cancelled");
    }
    solutionFiles.reset();
    {
        auto scan = co_await nav::scanProject(fs, scheduler, std::move(root), token);
        if (!scan) {
            files = nav::ProjectFiles();
            co_return std::unexpected(std::move(scan.error()));
        }
        if (token.stop_requested() || !files.setScanCancellable(*scan, token)) {
            files = nav::ProjectFiles();
            scan = fail(ErrorCode::Cancelled, "project scan cancelled");
            co_return std::unexpected(std::move(scan.error()));
        }
        // Drop the raw scan on this worker before the result reaches the GUI.
    }
    // Watcher traffic (including ignored build/log files) need not change the visible tree.
    // Compare on this worker, then preserve snapshot identity so the GUI need not reset.
    const auto sameNode = [&token](const nav::ProjectFiles::Node& a, const nav::ProjectFiles::Node& b) {
        return !token.stop_requested() && a.name == b.name && a.path == b.path && a.relative == b.relative &&
               a.directory == b.directory && a.parent == b.parent && a.children == b.children && a.row == b.row;
    };
    const auto samePath = [&token](const std::string& a, const std::string& b) {
        return !token.stop_requested() && a == b;
    };
    const bool unchanged = baseline && files.root() == baseline->root() &&
                           std::ranges::equal(files.nodes(), baseline->nodes(), sameNode) &&
                           std::ranges::equal(files.candidates(), baseline->candidates(), samePath) &&
                           std::ranges::equal(files.displayCandidates(), baseline->displayCandidates(), samePath);
    if (token.stop_requested())
        co_return fail(ErrorCode::Cancelled, "project scan cancelled");
    if (unchanged)
        co_return std::move(baseline);
    co_return std::make_shared<const nav::ProjectFiles>(std::move(files));
}

sched::Task<Result<void>> pumpWatch(io::IFileWatcher& watcher, std::string root, std::shared_ptr<WatchMailbox> mailbox,
                                    std::stop_token token) {
    auto watch = co_await watcher.watch(std::move(root), io::WatchOptions{}, token);
    if (!watch) {
        co_return std::unexpected(std::move(watch.error()));
    }
    for (;;) {
        auto batch = co_await (*watch)->next(token);
        if (!batch) {
            co_return std::unexpected(std::move(batch.error()));
        }
        mailbox->post(std::move(*batch));
    }
}

} // namespace

struct Workbench::WatchState {
    std::shared_ptr<WatchMailbox> mailbox = std::make_shared<WatchMailbox>();
    std::stop_source stop;
    std::optional<sched::JoinHandle<Result<void>>> handle;
};

Workbench::Workbench(ShellWindow& shell, WorkbenchServices services)
    : m_shell(shell), m_services(std::move(services)), m_findActions(shell.environment().registry) {
    setObjectName(QStringLiteral("workbench"));
    m_scanTimer = new QTimer(this);
    m_scanTimer->setObjectName(QStringLiteral("projectScanTimer"));
    m_scanTimer->setInterval(25);
    connect(m_scanTimer, &QTimer::timeout, this, [this] { pollScan(); });
    addToolWindow();
    addTodoToolWindow();
    registerActions();
    {
        std::vector<input::Binding> defaults(m_shell.environment().keymap.moduleDefaults().begin(),
                                             m_shell.environment().keymap.moduleDefaults().end());
        defaults.push_back({std::string(workbench_actions::kToggleBookmark),
                            {*input::parseSequence("F11", input::SyntaxMode::Strict)}});
        defaults.push_back({std::string(workbench_actions::kShowBookmarks),
                            {*input::parseSequence("Shift+F11", input::SyntaxMode::Strict)}});
        m_shell.environment().keymap.setModuleDefaults(std::move(defaults));
    }
    m_services.editors.setActions(BannerActions{
        [this](const std::string& path) { reloadFromDisk(path); },
        [this](const std::string& path) { overwriteDisk(path); },
        [this](const std::string& path) { static_cast<void>(showMerge(path)); },
    });
    m_services.saveManager.setExternalChangeListener(
        [this](const document::ExternalChange& change) { handleExternalChange(change); });
    EditorTabs& tabs = m_shell.tabs();
    connect(&tabs, &EditorTabs::statusUpdated, this, [this](IEditorView* editor) { onEditorStatus(editor); });
    connect(&tabs, &EditorTabs::currentEditorChanged, this,
            [this](IEditorView* editor) { onCurrentEditorChanged(editor); });
    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setObjectName(QStringLiteral("autosaveTimer"));
    m_autosaveTimer->setInterval(500);
    connect(m_autosaveTimer, &QTimer::timeout, this, [this] { pollAutosave(); });
    if (m_services.autosave != nullptr) {
        m_autosaveTimer->start();
    }
    m_shell.installEventFilter(this);
    if (QCoreApplication* app = QCoreApplication::instance(); app != nullptr) {
        app->installEventFilter(this);
    }
}

Workbench::~Workbench() {
    if (QCoreApplication* app = QCoreApplication::instance(); app != nullptr) {
        app->removeEventFilter(this);
    }
    m_shell.removeEventFilter(this);
    static_cast<void>(saveSession());
    stopWatching();
    if (m_initialFiles) {
        m_initialStop.request_stop();
        auto files = m_initialFiles->join();
        m_initialFiles.reset();
        retireSnapshot(m_services.scheduler, std::make_shared<nav::ProjectFiles>(std::move(files)));
    }
    if (m_scan) {
        m_scanStop.request_stop();
        auto result = m_scan->join();
        m_scan.reset();
        if (result) {
            retireSnapshot(m_services.scheduler, std::move(*result));
        }
    }
    if (m_todoScan) {
        m_todoStop.request_stop();
        static_cast<void>(m_todoScan->join());
        m_todoScan.reset();
    }
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles({});
    }
    retireSnapshot(m_services.scheduler, std::move(m_files));
    retireSolutionSnapshot(m_services.scheduler, std::move(m_solutionFiles));
    m_services.saveManager.setExternalChangeListener({});
    m_services.editors.setActions({});
    // Each view's destroyed() erases its entry: take the map first.
    const auto merges = std::exchange(m_merges, {});
    for (const auto& [path, view] : merges) {
        delete view.data();
    }
}

// --- Actions ---

void Workbench::addAction(std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                          std::function<bool()> enabled, std::vector<std::string> synonyms) {
    input::ActionSpec spec;
    spec.id = std::string(id);
    spec.name = std::string(name);
    spec.group = std::string(group);
    spec.synonyms = std::move(synonyms);
    spec.context = kWindowRule;
    const QPointer<Workbench> self(this);
    spec.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
        if (self != nullptr && run) {
            run();
        }
    };
    if (enabled) {
        spec.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
            return self != nullptr && enabled();
        };
    } else {
        spec.enabled = [self](const input::ActionContext&) { return self != nullptr; };
    }
    if (Result<input::ActionId> added = m_shell.environment().registry.add(std::move(spec)); !added) {
        log::warn("ui", "workbench action {}: {}", id, added.error().message());
    }
}

void Workbench::registerActions() {
    namespace a = workbench_actions;
    const auto hasEditor = [this] { return m_shell.tabs().currentEditor() != nullptr; };
    const auto hasProject = [this] { return !m_root.empty(); };
    const auto canCreateItem = [this] { return !defaultNewItemDirectory().empty(); };
    const auto canModifyItem = [this] { return !defaultTargetPathForModify().empty(); };
    addAction(a::kNewFile, "New File...", "File", [this] { promptAndCreateNewFile(); }, canCreateItem,
              {"create file", "add file"});
    addAction(a::kNewFolder, "New Folder...", "File", [this] { promptAndCreateNewFolder(); }, canCreateItem,
              {"create folder", "new directory"});
    addAction(a::kRename, "Rename...", "File", [this] { promptAndRenamePath(); }, canModifyItem,
              {"rename file", "rename folder"});
    addAction(a::kDelete, "Delete...", "File", [this] { promptAndDeletePath(); }, canModifyItem,
              {"delete file", "delete folder", "remove file"});
    addAction(a::kFindAction, "Find Action...", "Help", [this] { showFindAction(); }, {}, {"command", "action"});
    addAction(a::kSearchEverywhere, "Search Everywhere", "Navigate", [this] { showSearchEverywhere(); }, {},
              {"find", "search"});
    addAction(a::kGoToFile, "Go to File...", "Navigate", [this] { showGoToFile(); }, {}, {"open file", "file"});
    addAction(a::kRecentFiles, "Recent Files", "Navigate", [this] { showRecentFiles(); }, {}, {"switch file"});
    addAction(a::kRecentLocations, "Recent Locations", "Navigate", [this] { showRecentLocations(); }, {},
              {"recent places"});
    addAction(
        a::kLastEditLocation, "Last Edit Location", "Navigate", [this] { static_cast<void>(goToLastEditLocation()); },
        [this] { return m_lastEditPlace.has_value(); }, {"last edit", "previous edit"});
    addAction(a::kBack, "Back", "Navigate", [this] { goBack(); }, [this] { return m_history.canGoBack(); });
    addAction(a::kForward, "Forward", "Navigate", [this] { goForward(); }, [this] { return m_history.canGoForward(); });
    addAction(a::kToggleBookmark, "Toggle Bookmark", "Navigate", [this] { static_cast<void>(toggleBookmark()); },
              hasEditor, {"bookmark"});
    addAction(a::kShowBookmarks, "Show Bookmarks", "Navigate", [this] { static_cast<void>(showBookmarksPopup()); }, {},
              {"bookmarks", "list bookmarks"});
    addAction(
        a::kNextBookmark, "Next Bookmark", "Navigate", [this] { static_cast<void>(goToNextBookmark()); },
        [this] { return !m_bookmarks.empty(); }, {"next bookmark"});
    addAction(
        a::kPreviousBookmark, "Previous Bookmark", "Navigate", [this] { static_cast<void>(goToPreviousBookmark()); },
        [this] { return !m_bookmarks.empty(); }, {"previous bookmark"});
    addAction(a::kSaveAll, "Save All", "File", [this] { saveAll(); }, {}, {"save"});
    addAction(a::kSaveDocument, "Save Document", "File", [this] { static_cast<void>(saveDocument()); }, hasEditor,
              {"save file"});
    addAction(a::kSaveAs, "Save As...", "File",
              [this] {
                  if (m_services.saveAsHook) {
                      m_services.saveAsHook();
                      return;
                  }
                  const IEditorView* editor = m_shell.tabs().currentEditor();
                  if (editor == nullptr) {
                      return;
                  }
                  const std::string chosen = m_services.promptSaveAsPath
                                                 ? m_services.promptSaveAsPath(editor->path())
                                                 : toUtf8(QDir::toNativeSeparators(QFileDialog::getSaveFileName(
                                                       &m_shell, text("Save As"), nativePath(editor->path()))));
                  if (!chosen.empty()) {
                      static_cast<void>(saveDocumentAs(chosen));
                  }
              },
              hasEditor, {"save file as", "duplicate"});
    // Rider's Reload All from Disk (Ctrl+Alt+Y): every open file, not only the active one.
    addAction(a::kReloadFromDisk, "Reload All from Disk", "File", [this] { reloadAllFromDisk(); }, {},
              {"refresh", "revert", "synchronize"});
    addAction(
        a::kCloseProject, "Close Project", "File",
        [this] {
            if (m_services.closeProjectHook) {
                m_services.closeProjectHook();
            } else {
                static_cast<void>(closeProject());
            }
        },
        hasProject);
    addAction(a::kFileSystemWindow, "File System", "View",
              [this] { m_shell.toolWindows().show(kFileSystemToolWindow); }, {}, {"files", "folder", "tree"});
    addAction(a::kTodoWindow, "TODO", "View",
              [this] {
                  const bool alreadyVisible = m_shell.toolWindows().isVisible(kTodoToolWindow);
                  m_shell.toolWindows().show(kTodoToolWindow);
                  if (alreadyVisible) {
                      refreshTodoItems();
                  }
              },
              {}, {"todo", "fixme", "tasks"});
    // The settings files' stand-in actions, described by frost::settings (settingsActions()).
    for (const settings::SettingsActionInfo& info : settings::settingsActions()) {
        const settings::Scope scope = info.scope;
        std::vector<std::string> synonyms;
        switch (scope) {
        case settings::Scope::ProjectShared: synonyms = {".frost/settings.json", "shared settings"}; break;
        case settings::Scope::ProjectPersonal: synonyms = {".frost/workspace.json", "workspace settings"}; break;
        default: synonyms = {"settings.json", "preferences"}; break;
        }
        std::function<bool()> enabled;
        if (info.needsProject) {
            enabled = hasProject;
        } else {
            enabled = [this] { return !m_services.applicationSettingsFile.empty(); };
        }
        addAction(
            info.id, info.name, info.group,
            [this, scope] {
                if (scope == settings::Scope::Application) {
                    openSettingsFile(m_services.applicationSettingsFile);
                } else if (auto file = projectSettingsFile(scope == settings::Scope::ProjectPersonal); file) {
                    openSettingsFile(*file);
                }
            },
            std::move(enabled), std::move(synonyms));
    }
}

void Workbench::addToolWindow() {
    auto* view = new FileSystemView(&m_shell, m_services.scheduler);
    view->hide();
    m_fileSystem = view;
    connect(view, &FileSystemView::fileActivated, this,
            [this](const QString& path) { static_cast<void>(openFile(toUtf8(path))); });
    connect(view, &FileSystemView::newFileRequested, this,
            [this](const QString& directory) { promptAndCreateNewFile(toUtf8(directory)); });
    connect(view, &FileSystemView::newFolderRequested, this,
            [this](const QString& directory) { promptAndCreateNewFolder(toUtf8(directory)); });
    connect(view, &FileSystemView::renameRequested, this,
            [this](const QString& path) { promptAndRenamePath(toUtf8(path)); });
    connect(view, &FileSystemView::deleteRequested, this,
            [this](const QString& path) { promptAndDeletePath(toUtf8(path)); });
    ToolWindowSpec spec;
    spec.id = std::string(kFileSystemToolWindow);
    spec.title = text("File System");
    spec.side = ToolWindowSide::Left;
    spec.iconId = std::string(icons::kNodesFolder);
    spec.shortcutText = shortcutText(workbench_actions::kFileSystemWindow);
    const QPointer<FileSystemView> content(view);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "File System tool window: {}", added.error().message());
    }
}

void Workbench::addTodoToolWindow() {
    auto* list = new QListWidget(&m_shell);
    list->setObjectName(QStringLiteral("todoToolWindowList"));
    list->setFrameShape(QFrame::NoFrame);
    list->setUniformItemSizes(true);
    list->hide();
    m_todoList = list;
    const auto openTodoItem = [this](QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        const QVariantMap map = item->data(Qt::UserRole).toMap();
        const QString path = map.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            return;
        }
        const int line = map.value(QStringLiteral("line"), 1).toInt();
        const int column = map.value(QStringLiteral("column"), 1).toInt();
        static_cast<void>(openFile(toUtf8(path), LinePosition{line, column}));
    };
    connect(list, &QListWidget::itemActivated, this, openTodoItem);
    connect(list, &QListWidget::itemClicked, this, openTodoItem);
    connect(&m_shell.toolWindows(), &ToolWindowManager::visibilityChanged, this,
            [this](const QString& id, bool visible) {
                if (visible && id == fromUtf8(kTodoToolWindow)) {
                    refreshTodoItems();
                }
            });
    ToolWindowSpec spec;
    spec.id = std::string(kTodoToolWindow);
    spec.title = text("TODO");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kStatusInfo);
    spec.shortcutText = shortcutText(workbench_actions::kTodoWindow);
    const QPointer<QListWidget> content(list);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "TODO tool window: {}", added.error().message());
    }
}

QString Workbench::shortcutText(std::string_view actionId) const {
    const ShellEnvironment& env = m_shell.environment();
    const std::optional<input::ActionId> id = env.registry.find(actionId);
    if (!id) {
        return {};
    }
    const std::optional<input::Sequence> shortcut = env.keymap.primaryShortcut(*id);
    return shortcut ? fromUtf8(input::toDisplayText(*shortcut, env.keymap.layout(), env.keymap.platform())) : QString();
}

// --- Project ---

Result<void> Workbench::openProject(const std::string& path) {
    const std::string canonical = m_services.documents.canonicalize(path);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    if (status->type == io::FileType::Directory) {
        return openRoot(canonical, {}, {});
    }
    const QString extension = lowerExtension(canonical);
    if (extension != QStringLiteral(".sln") && extension != QStringLiteral(".slnx")) {
        return fail(ErrorCode::InvalidArgument, "'" + canonical + "' is not a folder or a solution");
    }
    auto model = project::SolutionModel::load(m_services.fs, canonical);
    if (!model) {
        return std::unexpected(std::move(model.error()));
    }
    return openSolution(*model);
}

Result<void> Workbench::openSolution(const project::SolutionModel& solution) {
    std::string path = m_services.documents.canonicalize(solution.path());
    std::string root(nav::parentOf(path));
    return openRoot(std::move(root), std::move(path), nav::solutionFiles(solution));
}

Result<void> Workbench::refreshSolution(const project::SolutionModel& solution) {
    if (m_solution.empty() || m_services.documents.canonicalize(solution.path()) != m_solution)
        return fail(ErrorCode::InvalidArgument, "the solution being refreshed is not open");
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_scanPublished = false;
    auto old = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>(nav::solutionFiles(solution));
    retireSolutionSnapshot(m_services.scheduler, std::move(old));
    startInitialFiles();
    refresh();
    return {};
}

Result<void> Workbench::openRoot(std::string root, std::string solution, std::vector<std::string> solutionFiles) {
    if (!m_root.empty() && !closeProject()) {
        return fail(ErrorCode::Cancelled, "a tab of the open project could not be closed");
    }
    m_solution = std::move(solution);
    setRoot(std::move(root), std::move(solutionFiles));
    if (m_services.sessions != nullptr) {
        if (auto session = m_services.sessions->load(m_root); session) {
            applySession(*session);
        }
        if (auto stored = m_services.sessions->setLastProject(m_root); !stored) {
            log::warn("ui", "cannot record the last project: {}", stored.error().message());
        }
    }
    return {};
}

void Workbench::setRoot(std::string root, std::vector<std::string> solutionFiles) {
    stopWatching();
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_todoStop.request_stop();
    m_todoRescanQueued = false;
    m_scanPublished = false;
    m_root = std::move(root);
    auto oldSolutionFiles = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>(std::move(solutionFiles));
    auto oldFiles = m_files;
    auto files = std::make_shared<nav::ProjectFiles>(m_root);
    if (!m_services.scheduler)
        files->setSolutionFiles(*m_solutionFiles);
    m_files = std::move(files);
    m_history.clear();
    m_recentFiles.clear();
    m_recentLocations.clear();
    m_lastEditPlace.reset();
    m_bookmarks.clear();
    m_cmakeTargets.clear();
    m_cmakeArguments.clear();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr) {
        m_todoList->clear();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
    retireSolutionSnapshot(m_services.scheduler, std::move(oldSolutionFiles));
    const std::string_view name = m_solution.empty() ? nav::baseName(m_root) : nav::baseName(m_solution);
    m_shell.setProject(fromUtf8(name));
    startInitialFiles();
    refresh();
    startWatching();
}

bool Workbench::closeProject() {
    static_cast<void>(saveSession());
    m_shell.tabs().closeAll();
    if (m_shell.tabs().totalCount() > 0) {
        return false; // A tab whose save failed was kept by the user; so is the project.
    }
    stopWatching();
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_todoStop.request_stop();
    m_todoRescanQueued = false;
    m_scanPublished = false;
    m_root.clear();
    m_solution.clear();
    m_lastEditPlace.reset();
    m_bookmarks.clear();
    auto oldSolutionFiles = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>();
    auto oldFiles = std::move(m_files);
    m_files = std::make_shared<nav::ProjectFiles>();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr) {
        m_todoList->clear();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
    retireSolutionSnapshot(m_services.scheduler, std::move(oldSolutionFiles));
    m_shell.setProject({});
    return true;
}

void Workbench::refresh() {
    if (m_root.empty()) {
        return;
    }
    if (m_services.scheduler == nullptr) {
        sched::SimExecutor sim;
        auto scan = sim.spawn(sched::Lane::Background,
                              prepareProjectFiles(m_services.fs, sim, m_root, m_solutionFiles, m_files, {}))
                        .join();
        applyScan(std::move(scan));
        return;
    }
    if (m_scanPending) {
        m_rescanQueued = true;
        return;
    }
    m_scanStop = std::stop_source();
    m_scanGeneration = m_projectGeneration;
    m_scan.emplace(m_services.scheduler->spawn(sched::Lane::Background,
                                               prepareProjectFiles(m_services.fs, *m_services.scheduler, m_root,
                                                                   m_solutionFiles, m_files, m_scanStop.get_token())));
    m_scanPending = true;
    m_scanTimer->start();
}

void Workbench::startInitialFiles() {
    if (!m_services.scheduler || m_initialFiles || m_root.empty() || m_solutionFiles->empty() || m_scanPublished)
        return;
    m_initialStop = std::stop_source();
    m_initialGeneration = m_projectGeneration;
    m_initialFiles.emplace(m_services.scheduler->spawn(
        sched::Lane::Background, prepareInitialFiles(m_root, m_solutionFiles, m_initialStop.get_token())));
    m_scanTimer->start();
}

void Workbench::pollInitialFiles() {
    if (!m_initialFiles || !m_initialFiles->ready())
        return;
    auto files = m_initialFiles->join();
    m_initialFiles.reset();
    if (m_initialGeneration == m_projectGeneration && !m_scanPublished && !m_initialStop.stop_requested()) {
        auto old = std::move(m_files);
        m_files = std::make_shared<nav::ProjectFiles>(std::move(files));
        if (m_fileSystem)
            m_fileSystem->setFiles(m_files);
        if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
            refreshTodoItems();
        }
        retireSnapshot(m_services.scheduler, std::move(old));
    } else {
        retireSnapshot(m_services.scheduler, std::make_shared<nav::ProjectFiles>(std::move(files)));
    }
    if (m_initialGeneration != m_projectGeneration)
        startInitialFiles();
}

void Workbench::pollScan() {
    if (m_scan && m_scan->ready()) {
        auto scan = m_scan->join();
        m_scan.reset();
        m_scanPending = false;
        if (m_scanGeneration == m_projectGeneration) {
            if (scan) {
                m_scanPublished = true;
                m_initialStop.request_stop();
            }
            applyScan(std::move(scan));
        } else {
            if (scan)
                retireSnapshot(m_services.scheduler, std::move(*scan));
            m_rescanQueued = true;
        }
        if (m_rescanQueued) {
            m_rescanQueued = false;
            refresh();
        }
    }
    pollInitialFiles();
    if (m_todoScan && m_todoScan->ready()) {
        auto entries = m_todoScan->join();
        m_todoScan.reset();
        if (m_todoScanGeneration == m_projectGeneration && !m_todoStop.stop_requested()) {
            applyTodoEntries(entries);
        }
        if (m_todoRescanQueued) {
            m_todoRescanQueued = false;
            refreshTodoItems();
        }
    }
    if (!m_scan && !m_initialFiles && !m_todoScan)
        m_scanTimer->stop();
}

void Workbench::applyScan(Result<std::shared_ptr<const nav::ProjectFiles>> scan) {
    if (!scan) {
        notifyError(text("Cannot list %1").arg(nativePath(m_root)), scan.error());
        return;
    }
    m_scanPublished = true;
    if (*scan == m_files) {
        if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible()) &&
            m_todoList->count() == 0) {
            refreshTodoItems();
        }
        return;
    }
    auto oldFiles = std::move(m_files);
    m_files = std::move(*scan);
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
}

void Workbench::startWatching() {
    if (m_services.watcher == nullptr || m_services.scheduler == nullptr || m_root.empty()) {
        return;
    }
    m_watch = std::make_unique<WatchState>();
    m_watch->mailbox->target = this;
    m_watch->handle.emplace(m_services.scheduler->spawn(
        sched::Lane::Background, pumpWatch(*m_services.watcher, m_root, m_watch->mailbox, m_watch->stop.get_token())));
}

void Workbench::stopWatching() {
    if (!m_watch) {
        return;
    }
    {
        const std::scoped_lock lock(m_watch->mailbox->mutex);
        m_watch->mailbox->target = nullptr;
    }
    m_watch->stop.request_stop();
    if (m_watch->handle) {
        // The pump ends with Cancelled (or the watch's own error); either way it is over.
        static_cast<void>(m_watch->handle->join());
    }
    m_watch.reset();
}

// --- Opening and navigation ---

nav::Place Workbench::currentPlace() const {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return {};
    }
    const EditorStatus status = editor->status();
    return nav::Place{editor->path(), status.line, status.column};
}

Result<int> Workbench::openFile(const std::string& path, std::optional<LinePosition> at, bool statusColumn) {
    const nav::Place from = currentPlace();
    m_navigating = true;
    Result<int> opened = m_shell.openFile(path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        if (at) {
            if (statusColumn)
                editor->goToStatusPosition(at->line, at->column);
            else
                editor->goToLine(at->line, at->column);
        }
        const EditorStatus status = editor->status();
        const nav::Place to{editor->path(), status.line, status.column};
        if (!from.path.empty() && !nav::isNear(from, to, nav::NavigationHistory::kNearLines)) {
            m_history.recordJump(from);
            m_recentLocations.touch(from);
        }
        m_recentLocations.touch(to);
        m_recentFiles.touch(editor->path());
        m_lastPlaces[editor->path()] = to;
        m_activePath = editor->path();
        syncEditorBookmarks(editor->path());
        m_shell.focusEditor();
    }
    m_navigating = false;
    return opened;
}

bool Workbench::openPlace(const nav::Place& place) {
    m_navigating = true;
    Result<int> opened = m_shell.openFile(place.path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        editor->goToStatusPosition(place.line, place.column);
        m_lastPlaces[editor->path()] = place;
        m_activePath = editor->path();
        m_recentFiles.touch(editor->path());
        syncEditorBookmarks(editor->path());
        m_shell.focusEditor();
    } else {
        m_history.forget(place.path);
    }
    m_navigating = false;
    return opened.has_value();
}

bool Workbench::goBack() {
    const auto target = m_history.back(currentPlace());
    return target && openPlace(*target);
}

bool Workbench::goForward() {
    const auto target = m_history.forward(currentPlace());
    return target && openPlace(*target);
}

void Workbench::onEditorStatus(IEditorView* editor) {
    if (editor == nullptr || m_restoring) {
        return;
    }
    const EditorStatus status = editor->status();
    const nav::Place now{editor->path(), status.line, status.column};
    auto& last = m_lastPlaces[editor->path()];
    if (!m_navigating && !last.path.empty()) {
        if (m_history.noteCaretMove(last, now)) {
            m_recentLocations.touch(last);
        }
    }
    last = now;
}

void Workbench::onCurrentEditorChanged(IEditorView* editor) {
    const std::string next = editor != nullptr ? editor->path() : std::string();
    if (!m_navigating && !m_restoring && !m_activePath.empty() && next != m_activePath && !next.empty()) {
        // A switch through the tab bar or the switcher: the place left is a jump.
        if (const auto it = m_lastPlaces.find(m_activePath); it != m_lastPlaces.end()) {
            m_history.recordJump(it->second);
        }
    }
    if (!next.empty() && !m_restoring) {
        m_recentFiles.touch(next);
        if (!m_lastPlaces.contains(next)) {
            const EditorStatus status = editor->status();
            m_lastPlaces[next] = nav::Place{next, status.line, status.column};
        }
    }
    m_activePath = next;
}

// --- Popups ---

void Workbench::closeOpenPopup() {
    // The context a popup captures must be the main window's, not the popup's being replaced.
    if (Popup* open = m_shell.activePopup(); open != nullptr) {
        open->closePopup(Popup::CloseReason::Dismissed);
    }
}

std::vector<ListPopupItem> Workbench::fileItems(const std::string& pattern, std::size_t limit) const {
    // The project's candidates, then recent files outside it (opened from elsewhere).
    std::vector<std::string> absolute = m_files->candidates();
    std::vector<std::string> display = m_files->displayCandidates();
    const std::set<std::string, std::less<>> known(absolute.begin(), absolute.end());
    for (const std::string& recent : m_recentFiles.paths()) {
        if (!known.contains(recent)) {
            absolute.push_back(recent);
            display.push_back(m_root.empty() ? recent : nav::relativeTo(m_root, recent));
        }
    }
    std::vector<std::size_t> order;
    if (pattern.empty()) {
        // Rider's empty Go to File: the recent files first.
        std::set<std::size_t> taken;
        for (const std::string& recent : m_recentFiles.paths()) {
            if (const auto it = std::ranges::find(absolute, recent); it != absolute.end()) {
                const auto index = static_cast<std::size_t>(it - absolute.begin());
                if (taken.insert(index).second) {
                    order.push_back(index);
                }
            }
        }
        for (std::size_t i = 0; i < absolute.size() && order.size() < limit; ++i) {
            if (!taken.contains(i)) {
                order.push_back(i);
            }
        }
        if (order.size() > limit) {
            order.resize(limit);
        }
    } else {
        for (const search::RankedPath& ranked : search::rankPaths(pattern, display, limit)) {
            order.push_back(ranked.index);
        }
    }
    std::vector<ListPopupItem> items;
    items.reserve(order.size());
    for (const std::size_t index : order) {
        const std::string& path = absolute[index];
        const std::string_view name = nav::baseName(path);
        const std::string_view folder = nav::parentOf(display[index] == path ? path : display[index]);
        ListPopupItem item;
        item.text = fromUtf8(name);
        item.detail = nativePath(folder);
        item.icon = fileIcon(name);
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<ListPopupItem> Workbench::actionItems(const std::string& query, const input::ActionContext& context,
                                                  std::size_t limit) const {
    std::vector<ListPopupItem> items;
    if (QString::fromUtf8(query).trimmed().isEmpty()) {
        return items;
    }
    const ShellEnvironment& env = m_shell.environment();
    input::FindActionOptions options;
    options.limit = limit;
    options.includeDisabled = true;
    for (const input::FindActionHit& hit : m_findActions.query(query, env.keymap.bindings(), context, options)) {
        const input::ActionInfo& info = env.registry.info(hit.id);
        ListPopupItem item;
        item.text = qtr(kActionsContext, info.name);
        const QString shortcut = shortcutText(info.id);
        const QString group = info.group.empty() ? QString() : qtr(kActionsContext, info.group);
        item.detail =
            shortcut.isEmpty() ? group : (group.isEmpty() ? shortcut : shortcut + QStringLiteral("  ") + group);
        item.enabled = hit.enabled;
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("action"));
        data.insert(QStringLiteral("id"), fromUtf8(info.id));
        item.data = data;
        items.push_back(std::move(item));
    }
    return items;
}

void Workbench::acceptItem(const QVariant& data, const input::ActionContext& captured) {
    const QVariantMap map = data.toMap();
    const QString kind = map.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("file")) {
        std::optional<LinePosition> at;
        if (map.contains(QStringLiteral("line"))) {
            at = LinePosition{map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        }
        static_cast<void>(openFile(toUtf8(map.value(QStringLiteral("path")).toString()), at));
        return;
    }
    if (kind == QStringLiteral("place")) {
        const nav::Place place{toUtf8(map.value(QStringLiteral("path")).toString()),
                               map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        static_cast<void>(openFile(place.path, LinePosition{place.line, place.column}, true));
        return;
    }
    if (kind == QStringLiteral("action")) {
        input::ActionRegistry& registry = m_shell.environment().registry;
        const std::optional<input::ActionId> id = registry.find(toUtf8(map.value(QStringLiteral("id")).toString()));
        if (!id) {
            return;
        }
        // The focus keys from before the popup opened; level and trust as they are now.
        input::ActionContext context = captured;
        const input::ActionContext live = m_shell.actionContext();
        context.level = live.level;
        context.trusted = live.trusted;
        if (registry.tryInvoke(*id, context, input::InvokeInfo{input::InvokeSource::FindAction, false}) ==
            input::InvokeResult::Ran) {
            m_findActions.noteInvoked(*id);
        }
        m_shell.refreshActions();
    }
}

ListPopup* Workbench::showFindAction(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Find Action"), true);
    popup->setObjectName(QStringLiteral("findActionPopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    connect(popup, &ListPopup::filterTextChanged, popup, [this, popup, captured](const QString& value) {
        popup->setItems(actionItems(toUtf8(value), captured, 100));
    });
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showGoToFile(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Go to File"), true);
    popup->setObjectName(QStringLiteral("goToFilePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    const auto update = [this, popup](const QString& value) {
        const search::LocationQuery location = search::parseLocationQuery(toUtf8(value.trimmed()));
        std::vector<ListPopupItem> items = fileItems(location.pattern, 50);
        if (location.line) {
            for (ListPopupItem& item : items) {
                item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()), location.line,
                                     location.column);
            }
        }
        popup->setItems(std::move(items));
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showSearchEverywhere(const QString& query, SearchEverywhereCategory category) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    const auto categoryTitle = [](SearchEverywhereCategory cat) -> QString {
        switch (cat) {
        case SearchEverywhereCategory::All:
            return text("Search Everywhere  [All]  Classes  Files  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Classes:
            return text("Search Everywhere  All  [Classes]  Files  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Files:
            return text("Search Everywhere  All  Classes  [Files]  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Symbols:
            return text("Search Everywhere  All  Classes  Files  [Symbols]  Actions  (Tab)");
        case SearchEverywhereCategory::Actions:
            return text("Search Everywhere  All  Classes  Files  Symbols  [Actions]  (Tab)");
        }
        return text("Search Everywhere");
    };
    auto* popup = new ListPopup(&m_shell, categoryTitle(category), true);
    popup->setObjectName(QStringLiteral("searchEverywherePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    popup->setScopeTabs({text("All"), text("Classes"), text("Files"), text("Symbols"), text("Actions")},
                        static_cast<int>(category));
    auto currentCategory = std::make_shared<SearchEverywhereCategory>(category);
    auto queryGen = std::make_shared<std::uint64_t>(0);
    const auto buildSyncItems = [this, captured](const search::LocationQuery& location, const std::string& utf8,
                                                 SearchEverywhereCategory cat,
                                                 const std::vector<WorkbenchSymbol>* symbols) {
        std::vector<ListPopupItem> items;
        if (symbols != nullptr && !symbols->empty() &&
            (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Classes ||
             cat == SearchEverywhereCategory::Symbols)) {
            ListPopupItem header;
            header.text = cat == SearchEverywhereCategory::Classes ? text("Classes") : text("Symbols");
            header.header = true;
            items.push_back(std::move(header));
            const std::size_t limit = cat == SearchEverywhereCategory::All ? 8 : 50;
            for (std::size_t i = 0; i < symbols->size() && i < limit; ++i) {
                const WorkbenchSymbol& sym = (*symbols)[i];
                ListPopupItem item;
                item.text = fromUtf8(sym.name);
                const QString fileRel = nativePath(m_root.empty() ? sym.path : nav::relativeTo(m_root, sym.path));
                item.detail =
                    sym.container.empty() ? fileRel : fromUtf8(sym.container) + QStringLiteral("  ") + fileRel;
                item.icon = fileIcon(nav::baseName(sym.path));
                QVariantMap data;
                data.insert(QStringLiteral("kind"), QStringLiteral("place"));
                data.insert(QStringLiteral("path"), fromUtf8(sym.path));
                data.insert(QStringLiteral("line"), sym.line);
                data.insert(QStringLiteral("column"), sym.column);
                item.data = data;
                items.push_back(std::move(item));
            }
        }
        if (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Files) {
            const std::size_t limit = cat == SearchEverywhereCategory::Files ? 50 : 8;
            std::vector<ListPopupItem> files = fileItems(location.pattern, limit);
            if (!files.empty()) {
                ListPopupItem header;
                header.text = text("Files");
                header.header = true;
                items.push_back(std::move(header));
                for (ListPopupItem& item : files) {
                    if (location.line) {
                        item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()),
                                             location.line, location.column);
                    }
                    items.push_back(std::move(item));
                }
            }
        }
        if (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Actions) {
            const std::size_t limit = cat == SearchEverywhereCategory::Actions ? 50 : 8;
            std::vector<ListPopupItem> actions = actionItems(utf8, captured, limit);
            if (!actions.empty()) {
                ListPopupItem header;
                header.text = text("Actions");
                header.header = true;
                items.push_back(std::move(header));
                std::ranges::move(actions, std::back_inserter(items));
            }
        }
        return items;
    };
    const auto update = [this, popup, currentCategory, queryGen, buildSyncItems](const QString& value) {
        const std::uint64_t gen = ++(*queryGen);
        const std::string utf8 = toUtf8(value.trimmed());
        const search::LocationQuery location = search::parseLocationQuery(utf8);
        const SearchEverywhereCategory cat = *currentCategory;
        popup->setItems(buildSyncItems(location, utf8, cat, nullptr));
        const bool wantsSymbols = (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Classes ||
                                   cat == SearchEverywhereCategory::Symbols) &&
                                  !location.pattern.empty() && static_cast<bool>(m_services.queryWorkspaceSymbols);
        if (wantsSymbols) {
            const QPointer<ListPopup> safePopup(popup);
            const bool classesOnly = (cat == SearchEverywhereCategory::Classes);
            m_services.queryWorkspaceSymbols(
                location.pattern, classesOnly,
                [safePopup, queryGen, gen, location, utf8, cat, classesOnly,
                 buildSyncItems](std::vector<WorkbenchSymbol> symbols) {
                    if (safePopup == nullptr || *queryGen != gen) {
                        return;
                    }
                    if (classesOnly) {
                        std::erase_if(symbols, [](const WorkbenchSymbol& s) { return !s.isClassLike; });
                    }
                    safePopup->setItems(buildSyncItems(location, utf8, cat, &symbols));
                });
        }
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    const auto switchCategory = [popup, currentCategory, categoryTitle, update](int next) {
        if (next < 0 || next >= 5) {
            return;
        }
        *currentCategory = static_cast<SearchEverywhereCategory>(next);
        popup->setTitle(categoryTitle(*currentCategory));
        popup->setActiveScopeTab(next);
        update(popup->filterField() != nullptr ? popup->filterField()->text() : QString());
    };
    connect(popup, &ListPopup::tabPressed, popup, [currentCategory, switchCategory](bool backward) {
        const int count = 5;
        const int cur = static_cast<int>(*currentCategory);
        const int next = backward ? (cur + count - 1) % count : (cur + 1) % count;
        switchCategory(next);
    });
    connect(popup, &ListPopup::scopeTabClicked, popup, switchCategory);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showRecentFiles() {
    closeOpenPopup();
    const auto paths = m_recentFiles.paths();
    if (paths.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Files"), true);
    popup->setObjectName(QStringLiteral("recentFilesPopup"));
    std::vector<ListPopupItem> items;
    for (const std::string& path : paths) {
        ListPopupItem item;
        item.text = fromUtf8(nav::baseName(path));
        item.detail = nativePath(nav::parentOf(m_root.empty() ? path : nav::relativeTo(m_root, path)));
        item.icon = fileIcon(nav::baseName(path));
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    // The current file is first; the useful default is the one before it.
    const bool currentFirst = paths.front() == m_activePath;
    popup->setCurrentItem(currentFirst && paths.size() > 1 ? 1 : 0);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

ListPopup* Workbench::showRecentLocations() {
    closeOpenPopup();
    const auto places = m_recentLocations.places();
    if (places.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Locations"), true);
    popup->setObjectName(QStringLiteral("recentLocationsPopup"));
    std::vector<ListPopupItem> items;
    for (const nav::Place& place : places) {
        ListPopupItem item;
        item.text = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(place.path))).arg(place.line);
        item.detail = nativePath(nav::parentOf(m_root.empty() ? place.path : nav::relativeTo(m_root, place.path)));
        item.icon = fileIcon(nav::baseName(place.path));
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("place"));
        data.insert(QStringLiteral("path"), fromUtf8(place.path));
        data.insert(QStringLiteral("line"), place.line);
        data.insert(QStringLiteral("column"), place.column);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

void Workbench::recordEditLocation(const std::string& path, int line, int column) {
    if (path.empty()) {
        return;
    }
    m_lastEditPlace = nav::Place{m_services.documents.canonicalize(path), std::max(1, line), std::max(1, column)};
    m_shell.refreshActions();
}

bool Workbench::goToLastEditLocation() {
    if (!m_lastEditPlace || m_lastEditPlace->path.empty()) {
        return false;
    }
    return openFile(m_lastEditPlace->path, LinePosition{m_lastEditPlace->line, m_lastEditPlace->column}, true)
        .has_value();
}

void Workbench::syncEditorBookmarks(const std::string& path) {
    std::vector<int> lines;
    if (const auto it = m_bookmarks.find(path); it != m_bookmarks.end()) {
        lines.assign(it->second.begin(), it->second.end());
    }
    m_services.editors.setBookmarks(path, lines);
    for (EditorTabGroup* group : m_shell.tabs().groups()) {
        for (int i = 0; i < group->count(); ++i) {
            if (IEditorView* editor = group->editor(i); editor != nullptr && editor->path() == path) {
                editor->setBookmarks(lines);
            }
        }
    }
}

bool Workbench::toggleBookmark() {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr || editor->path().empty()) {
        return false;
    }
    return toggleBookmarkAt(editor->path(), editor->status().line);
}

bool Workbench::toggleBookmarkAt(const std::string& path, int line) {
    if (path.empty() || line < 1) {
        return false;
    }
    const std::string canonical = m_services.documents.canonicalize(path);
    auto& lines = m_bookmarks[canonical];
    if (lines.contains(line)) {
        lines.erase(line);
        if (lines.empty()) {
            m_bookmarks.erase(canonical);
        }
    } else {
        lines.insert(line);
    }
    syncEditorBookmarks(canonical);
    m_shell.refreshActions();
    return true;
}

std::vector<nav::Place> Workbench::bookmarks() const {
    std::vector<nav::Place> all;
    for (const auto& [path, lines] : m_bookmarks) {
        for (const int line : lines) {
            all.push_back(nav::Place{path, line, 1});
        }
    }
    return all;
}

bool Workbench::goToNextBookmark() {
    const std::vector<nav::Place> all = bookmarks();
    if (all.empty()) {
        return false;
    }
    const nav::Place cur = currentPlace();
    for (const nav::Place& bm : all) {
        if (bm.path > cur.path || (bm.path == cur.path && bm.line > cur.line)) {
            return openFile(bm.path, LinePosition{bm.line, bm.column}).has_value();
        }
    }
    return openFile(all.front().path, LinePosition{all.front().line, all.front().column}).has_value();
}

bool Workbench::goToPreviousBookmark() {
    const std::vector<nav::Place> all = bookmarks();
    if (all.empty()) {
        return false;
    }
    const nav::Place cur = currentPlace();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->path < cur.path || (it->path == cur.path && it->line < cur.line)) {
            return openFile(it->path, LinePosition{it->line, it->column}).has_value();
        }
    }
    return openFile(all.back().path, LinePosition{all.back().line, all.back().column}).has_value();
}

ListPopup* Workbench::showBookmarksPopup() {
    closeOpenPopup();
    const std::vector<nav::Place> all = bookmarks();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Bookmarks"), true);
    popup->setObjectName(QStringLiteral("bookmarksPopup"));
    std::vector<ListPopupItem> items;
    for (const nav::Place& place : all) {
        ListPopupItem item;
        item.text = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(place.path))).arg(place.line);
        item.detail = nativePath(nav::parentOf(m_root.empty() ? place.path : nav::relativeTo(m_root, place.path)));
        item.icon = fileIcon(nav::baseName(place.path));
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("place"));
        data.insert(QStringLiteral("path"), fromUtf8(place.path));
        data.insert(QStringLiteral("line"), place.line);
        data.insert(QStringLiteral("column"), place.column);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

void Workbench::applyTodoEntries(const std::vector<TodoEntry>& entries) {
    if (m_todoList == nullptr) {
        return;
    }
    m_todoList->setUpdatesEnabled(false);
    m_todoList->clear();
    for (const TodoEntry& entry : entries) {
        auto* item = new QListWidgetItem(QStringLiteral("%1:%2  %3")
                                             .arg(nativePath(entry.relativePath))
                                             .arg(entry.line)
                                             .arg(fromUtf8(entry.snippet).trimmed()));
        item->setIcon(fileIcon(nav::baseName(entry.path)));
        QVariantMap data;
        data.insert(QStringLiteral("path"), fromUtf8(entry.path));
        data.insert(QStringLiteral("line"), entry.line);
        data.insert(QStringLiteral("column"), entry.column);
        item->setData(Qt::UserRole, data);
        m_todoList->addItem(item);
    }
    m_todoList->setUpdatesEnabled(true);
}

void Workbench::refreshTodoItems() {
    if (m_todoList == nullptr) {
        return;
    }

    static constexpr std::size_t kMaxCandidateFiles = 2048;
    static constexpr std::size_t kMaxTodoItems = 1500;
    static constexpr std::uint64_t kMaxFileBytes = 256ULL * 1024ULL;

    std::vector<std::string> candidatePaths;
    std::set<std::string, std::less<>> seen;
    const auto addCandidate = [&](const std::string& path) {
        if (candidatePaths.size() >= kMaxCandidateFiles || !isTodoScannablePath(path)) {
            return;
        }
        if (seen.insert(path).second) {
            candidatePaths.push_back(path);
        }
    };

    if (m_solutionFiles) {
        for (const std::string& path : *m_solutionFiles) {
            addCandidate(path);
        }
    }
    if (m_files) {
        for (const std::string& path : m_files->candidates()) {
            addCandidate(path);
        }
    }
    for (const auto& doc : m_services.saveManager.documents()) {
        if (doc != nullptr && !doc->path().empty()) {
            addCandidate(doc->path());
        }
    }
    for (EditorTabGroup* group : m_shell.tabs().groups()) {
        if (group == nullptr) {
            continue;
        }
        for (int i = 0; i < group->count(); ++i) {
            if (const IEditorView* editor = group->editor(i); editor != nullptr && !editor->path().empty()) {
                addCandidate(editor->path());
            }
        }
    }

    std::map<std::string, std::string, std::less<>> openBuffers;
    for (const auto& doc : m_services.saveManager.documents()) {
        if (doc != nullptr && !doc->path().empty()) {
            openBuffers.emplace(doc->path(), doc->document().text().text());
        }
    }

    auto runScan = [root = m_root](io::IFileSystem& fs, const std::vector<std::string>& paths,
                                   const std::map<std::string, std::string, std::less<>>& buffers,
                                   std::stop_token token) -> std::vector<TodoEntry> {
        std::vector<TodoEntry> entries;
        for (const std::string& path : paths) {
            if (token.stop_requested() || entries.size() >= kMaxTodoItems) {
                break;
            }
            std::string content;
            if (const auto it = buffers.find(path); it != buffers.end()) {
                content = it->second;
            } else {
                auto status = fs.status(path);
                if (!status || status->type != io::FileType::Regular || status->size == 0 ||
                    status->size > kMaxFileBytes) {
                    continue;
                }
                auto read = fs.readFile(path);
                if (!read) {
                    continue;
                }
                content = std::move(*read);
            }
            if (!mayContainTodoTag(content)) {
                continue;
            }
            const std::string rel = root.empty() ? path : nav::relativeTo(root, path);
            std::size_t pos = 0;
            int lineNum = 1;
            while (pos <= content.size() && entries.size() < kMaxTodoItems) {
                const std::size_t nl = content.find('\n', pos);
                std::string_view line = nl == std::string::npos ? std::string_view(content).substr(pos)
                                                                : std::string_view(content).substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                if (const auto match = findTodoTag(line)) {
                    std::string_view snippet = line.substr(match->first);
                    if (snippet.size() > 240) {
                        snippet = snippet.substr(0, 240);
                    }
                    entries.push_back(
                        TodoEntry{path, rel, std::string(snippet), lineNum, static_cast<int>(match->first) + 1});
                }
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
                ++lineNum;
            }
        }
        return entries;
    };

    if (m_services.scheduler == nullptr) {
        applyTodoEntries(runScan(m_services.fs, candidatePaths, openBuffers, {}));
        return;
    }

    if (m_todoScan) {
        m_todoStop.request_stop();
        m_todoRescanQueued = true;
        m_scanTimer->start();
        return;
    }

    m_todoStop = std::stop_source();
    m_todoScanGeneration = m_projectGeneration;
    auto& fs = m_services.fs;
    const std::stop_token token = m_todoStop.get_token();
    auto task = [](io::IFileSystem& fileSystem, auto scanner, std::vector<std::string> paths,
                   std::map<std::string, std::string, std::less<>> buffers,
                   std::stop_token stopToken) -> sched::Task<std::vector<TodoEntry>> {
        co_return scanner(fileSystem, paths, buffers, stopToken);
    };
    m_todoScan.emplace(m_services.scheduler->spawn(
        sched::Lane::Background,
        task(fs, std::move(runScan), std::move(candidatePaths), std::move(openBuffers), token)));
    m_scanTimer->start();
}

// --- Sessions ---

nav::SessionState Workbench::captureSession() const {
    nav::SessionState state;
    state.root = m_root;
    state.cmakeTargets = m_cmakeTargets;
    state.cmakeArguments = m_cmakeArguments;
    const EditorTabs& tabs = m_shell.tabs();
    state.editorLayout = tabs.saveLayout();
    const auto groups = tabs.groups();
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const EditorTabGroup* group = groups[groupIndex];
        if (group == tabs.activeGroup())
            state.activeGroup = static_cast<int>(groupIndex);
        for (int i = 0; i < group->count(); ++i) {
            const IEditorView* editor = group->editor(i);
            const EditorViewState view = editor->viewState();
            if (group == tabs.activeGroup() && i == group->currentIndex())
                state.active = static_cast<int>(state.tabs.size());
            state.tabs.push_back(nav::SessionTab{editor->path(), group->isPinned(i), view.line, view.column,
                                                 view.topLine, view.foldTextHash, view.folds,
                                                 static_cast<int>(groupIndex), i == group->currentIndex()});
        }
    }
    ToolWindowManager& windows = m_shell.toolWindows();
    windows.syncSizesFromSplitters();
    for (const std::string& id : windows.ids()) {
        state.toolWindows.push_back(nav::SessionToolWindow{id, windows.isVisible(id), windows.size(id)});
    }
    state.recentFiles.assign(m_recentFiles.paths().begin(), m_recentFiles.paths().end());
    state.recentLocations.assign(m_recentLocations.places().begin(), m_recentLocations.places().end());
    return state;
}

void Workbench::applySession(const nav::SessionState& state) {
    m_restoring = true;
    m_cmakeTargets = state.cmakeTargets;
    m_cmakeArguments = state.cmakeArguments;
    EditorTabs& tabs = m_shell.tabs();
    if (!tabs.restoreLayout(state.editorLayout))
        (void)tabs.restoreLayout(R"({"group":0})");
    const auto groups = tabs.groups();
    std::vector<std::string> selected(groups.size());
    int activeGroup =
        state.activeGroup >= 0 && static_cast<std::size_t>(state.activeGroup) < groups.size() ? state.activeGroup : 0;
    std::string activePath;
    for (std::size_t i = 0; i < state.tabs.size(); ++i) {
        const nav::SessionTab& tab = state.tabs[i];
        const int groupIndex = tab.group >= 0 && static_cast<std::size_t>(tab.group) < groups.size() ? tab.group : 0;
        tabs.setActiveGroup(groups[static_cast<std::size_t>(groupIndex)]);
        Result<int> opened = m_shell.openFile(tab.path);
        if (!opened)
            continue;
        IEditorView* editor = tabs.editor(*opened);
        editor->restoreViewState(EditorViewState{tab.line, tab.column, tab.top, tab.foldTextHash, tab.folds});
        m_lastPlaces[editor->path()] = nav::Place{editor->path(), tab.line, tab.column};
        if (tab.pinned)
            tabs.setPinned(tabs.indexOf(editor->path()), true);
        if (tab.groupActive)
            selected[static_cast<std::size_t>(groupIndex)] = editor->path();
        if (static_cast<int>(i) == state.active) {
            activeGroup = groupIndex;
            activePath = editor->path();
        }
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (!selected[i].empty()) {
            tabs.setActiveGroup(groups[i]);
            tabs.activate(tabs.indexOf(selected[i]));
        }
    }
    tabs.setActiveGroup(groups[static_cast<std::size_t>(activeGroup)]);
    if (!activePath.empty()) {
        tabs.activate(tabs.indexOf(activePath));
        m_activePath = activePath;
    }
    ToolWindowManager& windows = m_shell.toolWindows();
    for (const nav::SessionToolWindow& window : state.toolWindows) {
        if (!windows.contains(window.id)) {
            continue;
        }
        if (window.size > 0) {
            windows.setSize(window.id, window.size);
        }
        if (window.visible) {
            windows.show(window.id, false);
        } else {
            windows.hide(window.id);
        }
    }
    m_recentFiles.assign(state.recentFiles);
    m_recentLocations.assign(state.recentLocations);
    m_restoring = false;
}

Result<void> Workbench::saveSession() {
    if (m_services.sessions == nullptr || m_root.empty()) {
        return {};
    }
    return m_services.sessions->save(captureSession());
}

bool Workbench::restoreLastProject() {
    if (m_services.sessions == nullptr) {
        return false;
    }
    const std::optional<std::string> last = m_services.sessions->lastProject();
    if (!last) {
        return false;
    }
    if (auto exists = m_services.fs.exists(*last); !exists || !*exists) {
        return false;
    }
    if (auto opened = m_services.openProjectHook ? m_services.openProjectHook(*last) : openProject(*last); !opened) {
        notifyError(text("Cannot reopen %1").arg(nativePath(*last)), opened.error());
        return false;
    }
    return true;
}

// --- Hot exit ---

std::size_t Workbench::offerRestore(storage::RecoveryScan scan) {
    std::set<std::string> keys;
    bool crashed = false;
    for (storage::RecoveredJournal& journal : scan.journals) {
        crashed = crashed || !journal.endedCleanly();
        if (auto entries = journal.entries(); entries) {
            for (const storage::JournalEntry& entry : *entries) {
                keys.insert(entry.state.documentKey);
            }
        } else {
            keys.insert(journal.path()); // Unreadable here; restore() reports it.
        }
    }
    if (keys.empty()) {
        for (storage::RecoveredJournal& journal : scan.journals) {
            static_cast<void>(std::move(journal).discard());
        }
        return 0;
    }
    m_recovery.emplace(std::move(scan));
    const auto count = static_cast<int>(keys.size());
    Notification notification;
    notification.title = crashed ? text("FrostIDE closed unexpectedly. Restore %1 unsaved files?").arg(count)
                                 : text("Restore %1 unsaved files from the last session?").arg(count);
    notification.text = text("Their unsaved changes were kept in the hot-exit journal.");
    notification.kind = NotificationKind::Warning;
    notification.sticky = true;
    const QPointer<Workbench> self(this);
    notification.actions.push_back(NotificationAction{text("Restore"), [self] {
                                                          if (self != nullptr) {
                                                              static_cast<void>(self->restoreRecovered());
                                                          }
                                                      }});
    notification.actions.push_back(NotificationAction{text("Discard"), [self] {
                                                          if (self != nullptr) {
                                                              self->discardRecovered();
                                                          }
                                                      }});
    m_recoveryNotification = m_shell.notifications().notify(std::move(notification));
    return keys.size();
}

document::RestoreReport Workbench::restoreRecovered() {
    if (!m_recovery) {
        return {};
    }
    document::RestoreReport report = m_services.saveManager.restore(*m_recovery);
    m_recovery.reset();
    int untitled = 0;
    for (document::FileDocument* document : report.restored) {
        if (document->untitled()) {
            ++untitled; // TODO(M2): untitled buffers get their own tabs with the real editor.
            continue;
        }
        static_cast<void>(openFile(document->path()));
    }
    QString summary = text("%1 files were restored.").arg(report.restored.size());
    if (!report.diskChanged.empty()) {
        summary += QStringLiteral(" ") +
                   text("%1 changed on disk since; compare before saving.").arg(report.diskChanged.size());
    }
    if (!report.deferred.empty()) {
        summary += QStringLiteral(" ") + text("%1 could not be reopened and were kept.").arg(report.deferred.size());
    }
    if (untitled > 0) {
        summary += QStringLiteral(" ") + text("%1 untitled buffers were restored without a tab.").arg(untitled);
    }
    m_shell.notifications().notify({text("Unsaved files restored"),
                                    summary,
                                    report.errors.empty() ? NotificationKind::Info : NotificationKind::Warning,
                                    std::nullopt,
                                    {}});
    return report;
}

void Workbench::discardRecovered() {
    if (!m_recovery) {
        return;
    }
    for (storage::RecoveredJournal& journal : m_recovery->journals) {
        if (auto discarded = std::move(journal).discard(); !discarded) {
            log::warn("ui", "cannot discard a hot-exit journal: {}", discarded.error().message());
        }
    }
    m_recovery.reset();
}

// --- Disk changes, merging and saving ---

void Workbench::applyFileChanges(const io::FileChangeBatch& batch) {
    if (m_services.filesChanged) {
        m_services.filesChanged(batch);
    }
    // The listener (handleExternalChange) reports each change the manager finds.
    static_cast<void>(m_services.saveManager.applyChanges(batch));
    bool projectChanged = false;
    for (const io::FileChange& change : batch.changes) {
        if (isUnder(m_root, change.path) && !nav::relativeTo(m_root, change.path).starts_with(".git/")) {
            projectChanged = true;
            break;
        }
    }
    if (projectChanged) {
        refresh();
    }
}

void Workbench::handleExternalChange(const document::ExternalChange& change) {
    if (change.document == nullptr) {
        return;
    }
    const std::string path = change.document->path();
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
    const QString name = nativePath(path);
    switch (change.kind) {
    case document::ExternalChangeKind::MergePending: {
        Notification notification;
        notification.title = text("%1 changed on disk").arg(name);
        notification.text = text("It has unsaved changes here and was changed by another program. Keep your "
                                 "version, take the one on disk, or compare them to merge.");
        notification.kind = NotificationKind::Warning;
        notification.sticky = true;
        // The document may be closed before the user answers: each action looks it up again.
        const QPointer<Workbench> self(this);
        notification.actions.push_back(NotificationAction{text("Keep my version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::KeepBuffer);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Take disk version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::TakeDisk);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                              if (self != nullptr) {
                                                                  static_cast<void>(self->showMerge(path));
                                                              }
                                                          }});
        m_shell.notifications().notify(std::move(notification));
        break;
    }
    case document::ExternalChangeKind::OutOfDate: {
        const bool tooLarge = change.error && change.error->code() == ErrorCode::OutOfRange;
        m_shell.notifications().notify(
            {tooLarge ? text("%1 is too large to reload").arg(name) : text("Cannot reload %1").arg(name),
             change.error ? fromUtf8(change.error->message()) : QString(),
             NotificationKind::Warning,
             std::nullopt,
             {}});
        break;
    }
    case document::ExternalChangeKind::Deleted:
        m_shell.notifications().notify({text("%1 was deleted on disk").arg(name),
                                        text("Saving creates it again."),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        break;
    case document::ExternalChangeKind::Failed:
        if (change.error) {
            notifyError(text("Cannot check %1 on disk").arg(name), *change.error);
        }
        break;
    case document::ExternalChangeKind::None:
    case document::ExternalChangeKind::Touched:
    case document::ExternalChangeKind::Reloaded: break;
    }
}

MergeView* Workbench::showMerge(const std::string& path) {
    if (const auto it = m_merges.find(path); it != m_merges.end() && it->second != nullptr) {
        it->second->raise();
        it->second->activateWindow();
        return it->second;
    }
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        m_shell.notifications().notify({text("Nothing to compare"),
                                        text("%1 has no pending change from disk.").arg(nativePath(path)),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        return nullptr;
    }
    auto created = MergeView::create(m_services.saveManager, *document, &m_shell);
    if (!created) {
        notifyError(text("Cannot compare %1").arg(nativePath(path)), created.error());
        return nullptr;
    }
    MergeView* view = *created;
    m_merges[document->path()] = view;
    const std::string key = document->path();
    connect(view, &MergeView::resolved, this, [this, key](const QString&, int) {
        m_services.editors.refreshBanners(key);
        m_services.documents.notifyChanged(key);
    });
    connect(view, &QObject::destroyed, this, [this, key] { m_merges.erase(key); });
    view->show();
    return view;
}

void Workbench::resolveExternalChange(const std::string& path, document::MergeDecision decision) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        return; // Closed, or decided elsewhere (the merge view, another notification).
    }
    if (auto resolved = m_services.saveManager.resolveExternalChange(*document, decision); !resolved) {
        notifyError(text("Cannot resolve the change of %1").arg(nativePath(path)), resolved.error());
    }
    if (const auto it = m_merges.find(document->path()); it != m_merges.end() && it->second != nullptr) {
        it->second->close();
    }
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
}

void Workbench::reloadAllFromDisk() {
    // The paths first: the listeners of each change run before the next check.
    std::vector<std::string> paths;
    for (const auto& document : m_services.saveManager.documents()) {
        if (!document->untitled()) {
            paths.push_back(document->path());
        }
    }
    for (const std::string& path : paths) {
        if (document::FileDocument* document = m_services.saveManager.find(path); document != nullptr) {
            handleExternalChange(m_services.saveManager.checkDisk(*document));
        }
    }
}

void Workbench::frameDeactivated() {
    if (m_services.autosave == nullptr) {
        return;
    }
    if (auto saved = m_services.autosave->frameDeactivated(); saved) {
        reportSaveAll(*saved, true);
    }
}

void Workbench::frameActivated() { reloadAllFromDisk(); }

void Workbench::reloadFromDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    handleExternalChange(m_services.saveManager.checkDisk(*document));
    m_services.editors.refreshBanners(path);
}

void Workbench::overwriteDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    document::SaveOptions options;
    options.overwriteExternalChanges = true;
    if (auto saved = m_services.saveManager.save(*document, options); !saved) {
        notifyError(text("Cannot save %1").arg(nativePath(path)), saved.error());
    } else {
        for (const Error& warning : saved->warnings)
            notifySaveWarning(path, warning);
    }
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
}

void Workbench::reportSaveAll(const document::SaveAllResult& result, bool quiet) {
    for (document::FileDocument* document : result.saved) {
        m_services.editors.refreshBanners(document->path());
        m_services.documents.notifyChanged(document->path());
    }
    if (!result.saved.empty() && m_todoList != nullptr &&
        (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    for (const document::FailedSave& failed : result.failed) {
        notifyError(text("Cannot save %1").arg(nativePath(failed.document->path())), failed.error);
    }
    for (const document::FailedSave& warning : result.warnings) {
        notifySaveWarning(warning.document->path(), warning.error);
    }
    if (quiet) {
        return;
    }
    for (const document::SkippedDocument& skipped : result.skipped) {
        const std::string path = skipped.document->path();
        const QString name = skipped.document->untitled() ? text("An untitled buffer") : nativePath(path);
        switch (skipped.reason) {
        case document::SkipReason::MergePending: {
            Notification notification;
            notification.title = text("%1 was not saved").arg(name);
            notification.text = text("It changed on disk; compare the versions first.");
            notification.kind = NotificationKind::Warning;
            const QPointer<Workbench> self(this);
            notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                                  if (self != nullptr) {
                                                                      static_cast<void>(self->showMerge(path));
                                                                  }
                                                              }});
            m_shell.notifications().notify(std::move(notification));
            break;
        }
        case document::SkipReason::OutOfDate:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("The file on disk changed; use Overwrite in the editor to save over it."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::NeedsConsent:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("It was loaded with replacement characters; save it explicitly to accept them."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::Untitled:
        case document::SkipReason::Deleted: break;
        }
    }
}

document::SaveAllResult Workbench::saveAll() {
    document::SaveAllResult result = m_services.saveManager.saveAll(document::SaveReason::SaveAll);
    reportSaveAll(result, false);
    return result;
}

Result<void> Workbench::saveDocument() {
    IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no document is active");
    }
    document::FileDocument* document = m_services.documents.find(editor->path());
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "'" + editor->path() + "' is not open as a document");
    }
    auto saved = m_services.saveManager.save(*document);
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
    if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    if (!saved) {
        notifyError(text("Cannot save %1").arg(nativePath(document->path())), saved.error());
        return std::unexpected(std::move(saved.error()));
    }
    for (const Error& warning : saved->warnings)
        notifySaveWarning(document->path(), warning);
    return {};
}

Result<void> Workbench::saveDocumentAs(std::string_view targetPath) {
    IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no document is active");
    }
    const std::string oldPath = editor->path();
    document::FileDocument* document = m_services.documents.find(oldPath);
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "'" + oldPath + "' is not open as a document");
    }
    if (targetPath.empty()) {
        return fail(ErrorCode::Cancelled, "Save As was cancelled");
    }
    const io::PathStyle style = nav::styleOf(oldPath.empty() ? targetPath : std::string_view(oldPath));
    auto normalized = io::normalizePath(targetPath, style);
    if (!normalized) {
        notifyError(text("Cannot save %1").arg(fromUtf8(targetPath)), normalized.error());
        return std::unexpected(std::move(normalized.error()));
    }
    std::string candidate = *normalized;
    if (auto existingCanonical = m_services.fs.canonicalPath(candidate); existingCanonical) {
        candidate = std::move(*existingCanonical);
    } else {
        const std::string_view parent = io::parentPath(candidate, style);
        const std::string_view name = io::fileName(candidate, style);
        if (!parent.empty() && !name.empty()) {
            const std::string canonicalParent = m_services.documents.canonicalize(parent);
            if (auto rejoined = io::joinPath(canonicalParent, name, style); rejoined) {
                candidate = std::move(*rejoined);
            }
        }
    }
    if (candidate == oldPath) {
        return saveDocument();
    }
    auto saved = m_services.saveManager.saveAs(*document, candidate);
    if (!saved) {
        m_services.editors.refreshBanners(oldPath);
        notifyError(text("Cannot save %1").arg(nativePath(candidate)), saved.error());
        return std::unexpected(std::move(saved.error()));
    }
    const std::string newPath = document->path();
    m_services.documents.renamePath(oldPath, newPath);
    m_services.editors.renamePath(oldPath, newPath);
    notifyPathRenamed(oldPath, newPath);
    m_services.documents.notifyChanged(newPath);
    for (const Error& warning : saved->warnings) {
        notifySaveWarning(newPath, warning);
    }
    return {};
}

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

void Workbench::openSettingsFile(const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (auto exists = m_services.fs.exists(path); exists && !*exists) {
        // A settings file starts as an empty object.
        static_cast<void>(m_services.fs.createDirectories(std::string(nav::parentOf(path))));
        if (auto written = m_services.fs.writeFileAtomic(path, "{}\n"); !written) {
            notifyError(text("Cannot create %1").arg(nativePath(path)), written.error());
            return;
        }
    }
    static_cast<void>(openFile(path));
}

std::optional<std::string> Workbench::projectSettingsFile(bool personal) const {
    if (m_root.empty()) {
        return std::nullopt;
    }
    auto layout = settings::ProjectLayout::forRoot(m_root, nav::styleOf(m_root));
    if (!layout) {
        return std::nullopt;
    }
    if (auto ensured = settings::ensureProjectLayout(m_services.fs, *layout); !ensured) {
        log::warn("ui", "cannot create {}: {}", layout->frostDirectory, ensured.error().message());
    }
    return personal ? layout->personalSettings : layout->sharedSettings;
}

void Workbench::notifyError(const QString& title, const Error& error) {
    m_shell.notifications().notify({title, fromUtf8(error.message()), NotificationKind::Error, std::nullopt, {}});
}

void Workbench::notifySaveWarning(const std::string& path, const Error& warning) {
    m_saveWarningNeedsReview = true;
    m_shell.notifications().notify({text("Saved %1 with a warning").arg(nativePath(path)),
                                    QString::fromStdString(warning.describe()),
                                    NotificationKind::Warning,
                                    std::nullopt,
                                    {}});
}

// --- Autosave and events ---

void Workbench::pollAutosave() {
    if (m_services.autosave == nullptr) {
        return;
    }
    const std::optional<IClock::SteadyTime> deadline = m_services.autosave->nextDeadline();
    if (!deadline || *deadline > m_services.clock.steadyNow()) {
        return;
    }
    const document::AutosaveController::PollResult result = m_services.autosave->poll();
    if (result.autosave) {
        reportSaveAll(*result.autosave, true);
    }
    if (result.journal) {
        for (const Error& error : result.journal->errors) {
            log::warn("ui", "hot-exit journal: {}", error.describe());
        }
    }
}

bool Workbench::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &m_shell && event->type() == QEvent::Close) {
        // Save while the window and its notification center can still tell the user about warnings.
        frameDeactivated();
        if (m_saveWarningNeedsReview) {
            m_saveWarningNeedsReview = false;
            m_shell.notifications().notify({text("Review save warnings before closing"),
                                            text("Files were saved, but follow-up work needs attention. "
                                                 "Review the save warnings in Notifications, then close again."),
                                            NotificationKind::Warning,
                                            true,
                                            {}});
            static_cast<QCloseEvent*>(event)->ignore();
            return true;
        }
    }
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::InputMethod:
        if (m_services.autosave != nullptr) {
            m_services.autosave->noteActivity();
        }
        break;
    default: break;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto* widget = qobject_cast<QWidget*>(watched);
        if ((mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton) && widget != nullptr &&
            widget->window() == &m_shell) {
            if (mouse->button() == Qt::BackButton) {
                goBack();
            } else {
                goForward();
            }
            return true;
        }
    }
    // Rider's frame hooks: autosave when the IDE loses focus, the disk check when it gets it back.
    if (watched == &m_shell && event->type() == QEvent::WindowDeactivate) {
        frameDeactivated();
    } else if (watched == &m_shell && event->type() == QEvent::WindowActivate) {
        frameActivated();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace frost::ui
