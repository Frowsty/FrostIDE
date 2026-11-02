// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the project side: opening solutions, folders and files,
// closing the project, the recent-projects list, and clangd (start, document sync, live
// diagnostics in Problems and the editors, the "clangd: indexing" background task).

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"

#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <functional>
#include <limits>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// The lower-cased extension of the last component, with its dot ("" for none).
std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    std::string out(name.substr(dot));
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return out;
}

// A database directory name for `root`: its last component and a hash of its document key.
std::string cacheNameFor(std::string_view root) {
    const std::size_t slash = root.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? root : root.substr(slash + 1));
    std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return std::format("{}-{:016x}", name.empty() ? std::string("root") : name,
                       std::hash<std::string>{}(lang::documentKey(root)));
}

} // namespace

bool isCFamilySource(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 18> kExtensions{".c",   ".cc",  ".cpp", ".cxx", ".c++", ".cppm",
                                                                  ".ixx", ".h",   ".hh",  ".hpp", ".hxx", ".h++",
                                                                  ".inl", ".ipp", ".tpp", ".m",   ".mm",  ".cu"};
    const std::string extension = extensionOf(path);
    return std::ranges::find(kExtensions, extension) != kExtensions.end();
}

bool isSolutionFile(std::string_view path) noexcept {
    const std::string extension = extensionOf(path);
    return extension == ".sln" || extension == ".slnx";
}

// --- Opening ---

Result<void> Workbench::openPath(std::string_view path, FileLocation at) {
    Result<std::string> canonical = canonicalPath(path);
    if (!canonical) {
        notify(text("Cannot open %1").arg(qs(path)), qs(canonical.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(canonical.error()));
    }
    Result<io::FileStatus> status = m_services.fs.status(*canonical);
    if (!status) {
        notify(text("Cannot open %1").arg(qs(*canonical)), qs(status.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    if (status->type == io::FileType::Directory) {
        return openFolder(*canonical);
    }
    if (isSolutionFile(*canonical)) {
        return openSolution(*canonical);
    }
    if (!m_files) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    Result<ui::IEditorView*> opened = m_files->open(*canonical, at);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return {};
}

Result<void> Workbench::openSolution(const std::string& path) {
    if (m_solution && lang::documentKey(m_solution->path()) == lang::documentKey(path)) {
        return {}; // Already the project.
    }
    Result<project::SolutionModel> loaded = project::SolutionModel::load(m_services.fs, path);
    if (!loaded) {
        notify(text("Cannot open %1").arg(qs(path)), qs(loaded.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(loaded.error()));
    }
    closeProject();
    auto model = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    m_solution = model;
    if (auto opened = m_build->openSolution(model); !opened) {
        // The explorer still shows the solution; building is unavailable.
        notify(text("The build service cannot open %1").arg(qs(model->name())), qs(opened.error().message()),
               ui::NotificationKind::Warning);
        if (m_explorer) {
            m_explorer->setSolution(model);
        }
    }
    setProject(model->directory(), qs(model->name()), path);
    if (m_window) {
        m_window->toolWindows().show(ui::tool_windows::kProject, false);
    }
    startClangd(model->directory());
    refreshAllMarkers();
    return {};
}

Result<void> Workbench::openFolder(const std::string& path) {
    // A folder with one solution opens that solution (.sln before .slnx of the same name).
    if (Result<std::vector<io::DirEntry>> entries = m_services.fs.listDirectory(path); entries) {
        std::vector<std::string> solutions;
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Regular && isSolutionFile(entry.name)) {
                solutions.push_back(entry.name);
            }
        }
        std::ranges::sort(solutions);
        const auto stem = [](const std::string& name) { return name.substr(0, name.find_last_of('.')); };
        const bool oneSolution = !solutions.empty() && std::ranges::all_of(solutions, [&](const std::string& name) {
            return stem(name) == stem(solutions.front());
        });
        if (oneSolution) {
            if (Result<std::string> solution = io::joinPath(path, solutions.front()); solution) {
                return openSolution(*solution);
            }
        }
    }
    closeProject();
    const std::string_view name = io::fileName(path);
    setProject(path, qs(name.empty() ? std::string_view(path) : name), path);
    if (m_window) {
        m_window->toolWindows().show(ui::tool_windows::kProject, false);
    }
    startClangd(path);
    return {};
}

void Workbench::setProject(std::string root, QString name, std::string recentPath) {
    m_projectRoot = std::move(root);
    if (auto added = addRecentProject(m_services.settings, recentPath); !added) {
        log::warn(kLog, "recent projects: {}", added.error().describe());
    }
    refreshRecent();
    if (m_window) {
        m_window->setProject(name);
        m_window->tabs().setEmptyStateWidget(nullptr);
        m_window->refreshActions();
    }
    emit projectChanged(name);
}

void Workbench::closeProject() {
    if (!hasProject()) {
        return;
    }
    if (m_window) {
        m_window->tabs().closeAll(); // Each dirty tab saves as it closes.
    }
    stopClangd();
    if (m_solution) {
        if (auto closed = m_build->closeSolution(); !closed) {
            log::warn(kLog, "closing the solution: {}", closed.error().describe());
        }
        if (m_explorer) {
            m_explorer->setSolution(nullptr);
        }
    }
    m_solution.reset();
    m_projectRoot.clear();
    if (m_window) {
        m_window->setProject(QString());
        m_window->tabs().setEmptyStateWidget(m_welcome);
        m_window->refreshActions();
    }
    refreshRecent();
    emit projectChanged(QString());
}

// --- clangd ---

void Workbench::startClangd(const std::string& root) {
    stopClangd();
    if (!m_options.clangd) {
        return;
    }
    const auto snapshot = m_services.settings.snapshot();
    Result<lang::ClangdLocation> found =
        lang::findClangd(m_services.fs, lang::defaultClangdSearch(lang::configuredClangdPath(*snapshot)));
    if (!found) {
        log::info(kLog, "no clangd, code insight is off: {}", found.error().describe());
        return;
    }
    if (!m_reactor) {
        Result<std::unique_ptr<io::Reactor>> reactor = io::Reactor::create(io::ReactorOptions{1, "frost-clangd-io"});
        if (!reactor) {
            notify(text("clangd cannot start"), qs(reactor.error().message()), ui::NotificationKind::Warning);
            return;
        }
        m_reactor = std::move(*reactor);
    }

    // An existing compile_commands.json first, then the commands synthesized from the solution.
    std::vector<std::shared_ptr<lang::ICompileCommandsProvider>> providers;
    providers.push_back(std::make_shared<lang::ExistingCompileCommandsProvider>(m_services.fs, root));
    if (m_solution && !m_options.compileCommandsCache.empty()) {
        if (Result<std::string> cache = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root)); cache) {
            lang::SynthesisOptions synthesis;
            if (const std::optional<std::size_t> active = m_build->activeConfigurationIndex();
                active && *active < m_build->configurations().size()) {
                synthesis.configuration = m_build->configurations()[*active].name();
            }
            std::function<Result<lang::MsvcEnvironment>()> msvc;
            if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
                msvc = [launcher = &m_services.launcher, fs = &m_services.fs] {
                    return lang::findMsvcEnvironment(*launcher, *fs);
                };
            }
            providers.push_back(std::make_shared<lang::SolutionCompileCommandsProvider>(
                m_services.fs, m_solution, *cache, std::move(synthesis), std::move(msvc)));
        }
    }
    m_compileCommands = std::make_shared<lang::FirstAvailableCompileCommandsProvider>(std::move(providers));

    lang::ClangdServiceOptions options;
    options.clangdPath = found->path;
    options.workspaceRoot = root;
    options.launch.jobs = m_options.clangdJobs;
    options.compileCommands = m_compileCommands;
    // Called on clangd's threads; the queued calls die with this object.
    options.onProgress = [this](const lang::ClangdProgress& progress) {
        QMetaObject::invokeMethod(this, [this, progress] { onClangdProgress(progress); }, Qt::QueuedConnection);
    };
    m_clangd = std::make_unique<lang::ClangdService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                     std::move(options));
    m_clangdListener = m_clangd->diagnostics().addListener([this](const lang::FileDiagnostics& diagnostics) {
        QMetaObject::invokeMethod(
            this, [this, file = diagnostics.file] { onLiveDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_clangd->start(); !started) {
        notify(text("clangd cannot start"), qs(started.error().message()), ui::NotificationKind::Warning);
        stopClangd();
        return;
    }
    for (DocumentEditorView* view : m_editors->views()) {
        syncClangdDocument(*view);
    }
}

void Workbench::stopClangd() {
    if (!m_clangd) {
        return;
    }
    m_clangd->diagnostics().removeListener(m_clangdListener);
    m_clangdListener = 0;
    m_pool->spawn(sched::Lane::UserVisible, m_clangd->stop()).join();
    m_clangd.reset();
    m_compileCommands.reset();
    if (m_indexing) {
        m_indexing->finish();
        m_indexing.reset();
    }
    m_indexToken.clear();
    m_liveMarkers.clear();
    for (DocumentEditorView* view : m_editors->views()) {
        view->editor().setRangeDecorations({});
        refreshMarkers(*view);
    }
    if (m_problems) {
        m_problems->model()->clear(ui::DiagnosticSource::Live);
    }
}

void Workbench::syncClangdDocument(DocumentEditorView& view) {
    if (!m_clangd || !isCFamilySource(view.path())) {
        return;
    }
    const text::Document& document = view.document().document();
    if (auto opened = m_clangd->openDocument(view.path(), document.text()); opened) {
        return;
    }
    // Already open: a ChangeSet that continues from no known version sends the whole text.
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_clangd->changeDocument(view.path(), whole, document.text());
}

void Workbench::onLiveDiagnostics(const std::string& file) {
    if (!m_clangd) {
        return;
    }
    const std::optional<lang::FileDiagnostics> set = m_clangd->diagnostics().forFile(file);
    if (!set || !set->stale) {
        // A stale publish (for a text the user has typed past) waits for the next one.
        const std::string key = lang::documentKey(file);
        std::vector<ui::LineMarker> markers;
        std::vector<ui::RangeDecoration> decorations;
        if (set) {
            for (const lang::LiveDiagnostic& d : set->diagnostics) {
                if (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning) {
                    continue;
                }
                const ui::MarkerSeverity severity = d.severity == lang::DiagnosticSeverity::Error
                                                        ? ui::MarkerSeverity::Error
                                                        : ui::MarkerSeverity::Warning;
                if (d.line > 0) {
                    markers.push_back(
                        ui::LineMarker{LineIndex{static_cast<std::uint32_t>(d.line - 1)}, severity, d.message});
                }
                if (d.range) {
                    decorations.push_back(ui::RangeDecoration{*d.range, severity, ui::UnderlineStyle::Wavy, d.message});
                }
            }
        }
        if (markers.empty()) {
            m_liveMarkers.erase(key);
        } else {
            m_liveMarkers[key] = std::move(markers);
        }
        if (DocumentEditorView* view = m_editors->find(file); view != nullptr) {
            view->editor().setRangeDecorations(std::move(decorations));
            refreshMarkers(*view);
        }
    }
    refreshLiveProblems();
}

void Workbench::refreshLiveProblems() {
    if (!m_problems || !m_clangd) {
        return;
    }
    std::vector<build::BuildDiagnostic> rows;
    for (const lang::LiveDiagnostic& d : m_clangd->diagnostics().problems()) {
        build::BuildDiagnostic row;
        row.diagnostic.severity = d.severity == lang::DiagnosticSeverity::Error     ? build::Severity::Error
                                  : d.severity == lang::DiagnosticSeverity::Warning ? build::Severity::Warning
                                                                                    : build::Severity::Message;
        row.diagnostic.code = d.code;
        row.diagnostic.message = d.message;
        row.diagnostic.file = d.file;
        row.diagnostic.line = d.line;
        row.diagnostic.column = d.column;
        row.path = d.file;
        row.jumpable = !d.file.empty() && d.line > 0;
        rows.push_back(std::move(row));
    }
    m_problems->model()->setProblems(ui::DiagnosticSource::Live, rows);
}

void Workbench::onClangdProgress(const lang::ClangdProgress& progress) {
    if (!m_clangd || !m_window) {
        return;
    }
    // clangd's background index reports as the work-done progress titled "indexing".
    const bool indexing = progress.token == m_indexToken || (progress.kind == lang::ClangdProgress::Kind::Begin &&
                                                             progress.title.find("index") != std::string::npos);
    if (!indexing) {
        return;
    }
    switch (progress.kind) {
    case lang::ClangdProgress::Kind::Begin:
        if (m_indexing) {
            m_indexing->finish();
        }
        m_indexToken = progress.token;
        m_indexing = m_window->backgroundTasks().start(text("clangd: indexing"), false);
        [[fallthrough]];
    case lang::ClangdProgress::Kind::Report:
        if (m_indexing) {
            if (progress.percentage) {
                m_indexing->setProgress(static_cast<double>(*progress.percentage) / 100.0);
            }
            if (!progress.message.empty()) {
                m_indexing->setDetail(qs(progress.message));
            }
        }
        return;
    case lang::ClangdProgress::Kind::End:
        if (m_indexing) {
            m_indexing->finish();
            m_indexing.reset();
        }
        m_indexToken.clear();
        return;
    }
}

} // namespace frost::app
