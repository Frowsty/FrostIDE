// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): clangd and the independent project evaluation, start on
// solutionCompileCommandsChain, one start per project open (FND-9a), restart on a configuration
// switch or a moved CMake database, document sync, live diagnostics in Problems and the editors and
// the "clangd: indexing" background task (split from workbench_project.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/cl_command_lines_provider.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/cmake_compile_commands.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"
#include "frost/project/project_model_cache.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/lang/resolve_context.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"

#include "cmake_integration.hpp"
#include "engine_binding_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "standalone_build_backend.hpp"
#include "unity_control.hpp"
#include "workbench_project_helpers.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

// Retain the service until its supervisor and process have finished stopping.
sched::Task<void> stopOwnedClangd(std::shared_ptr<lang::ClangdService> service, sched::Task<void> stopped) {
    (void)service;
    co_await std::move(stopped);
}

// CPP-3: rewrites the owned database (blocking I/O, so on a worker); retained by the task.
sched::Task<void> refreshOwnedCommands(std::shared_ptr<lang::CMakeCompileCommandsProvider> provider) {
    if (auto located = provider->locate(); !located)
        log::warn(kLog, "the owned compile commands were not rewritten: {}", located.error().describe());
    co_return;
}

sched::Task<void> stopOwnedProjectEvaluation(std::shared_ptr<lang::IndependentProjectEvaluation> service) {
    service->stopAndJoin();
    co_return;
}

// Ends the explorer's evaluation (and its ProjectHost) off the GUI thread.
sched::Task<void> stopOwnedSolutionEvaluation(std::shared_ptr<project::SolutionEvaluation> evaluation) {
    evaluation->stopAndJoin();
    co_return;
}

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// A database directory name for `root`: its last component and a hash of its document key.
std::string cacheNameFor(std::string_view root) {
    const std::size_t slash = root.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? root : root.substr(slash + 1));
    std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return std::format("{}-{:016x}", name.empty() ? std::string("root") : name,
                       std::hash<std::string>{}(lang::documentKey(root)));
}

} // namespace

// --- clangd ---

void Workbench::stopProjectEvaluation() {
    if (m_streamedProjects) {
        m_streamedProjects->invalidate();
        m_streamedProjects.reset();
    }
    std::erase_if(m_retiredProjectEvaluations, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
    m_evaluatedSolution.reset();
    m_evaluatedFrom.reset();
    m_evaluatedGeneration = 0;
    if (m_explorer)
        m_explorer->setEvaluationBanner({});
    if (m_solutionEvaluation) {
        auto evaluation = std::move(m_solutionEvaluation);
        evaluation->requestStop();
        m_retiredProjectEvaluations.push_back(
            m_pool->spawn(sched::Lane::Background, stopOwnedSolutionEvaluation(std::move(evaluation))));
    }
    if (!m_projectEvaluation)
        return;
    auto old = std::move(m_projectEvaluation);
    old->requestStop();
    m_retiredProjectEvaluations.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedProjectEvaluation(std::move(old))));
}

void Workbench::startProjectEvaluation(const std::string& root) {
    if (m_shutDown)
        return;
    if (m_buildService->projectEditInProgress()) {
        m_projectEvaluationDeferred = true;
        return;
    }
    m_projectEvaluationDeferred = false;
    if (m_explorer) {
        m_explorer->endSpeedSearch();
        if (m_solution)
            m_explorer->setSolution(m_solution);
    }
    stopProjectEvaluation();
    if (m_solution && m_solution->approximate())
        return; // Trust does not make Linux static parsing into an MSBuild evaluation.
    const auto admission = m_services.trust.snapshot(root);
    m_projectTrustRevision = admission.revision;
    std::string configuration;
    if (const auto active = m_build->activeConfigurationIndex(); active && *active < m_build->configurations().size())
        configuration = m_build->configurations()[*active].name();
    const auto locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
        auto instance = build::findMsBuild(*launcher, *fs);
        if (!instance)
            return std::unexpected(std::move(instance.error()));
        if (instance->msbuildPath.empty())
            return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
        return instance->msbuildPath;
    };
    const auto trustedNow = [trust = &m_services.trust, root, revision = admission.revision] {
        const auto current = trust->snapshot(root);
        return current.trusted && current.revision == revision;
    };
    // SLN-2: the Solution Explorer's items from ProjectHost (MSBuild.exe when it is unavailable).
    if (m_solution && admission.trusted && m_options.projectHost) {
        project::SolutionEvaluationOptions evaluation;
        evaluation.hostPath = m_options.projectEditHostPath;
        if (evaluation.hostPath.empty())
            evaluation.hostPath = QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("project-host/ProjectHostVsProbe.exe"))
                                      .toStdString();
        if (auto normalized = io::normalizePath(evaluation.hostPath))
            evaluation.hostPath = std::move(*normalized);
        const auto settings = m_services.settings.snapshot();
        evaluation.msbuildPath =
            !m_options.msbuildPath.empty() ? m_options.msbuildPath : settings->get(settings::key::toolsMsbuildPath);
        evaluation.locateMsBuild = locateMsBuild;
        evaluation.configuration = configuration;
        evaluation.trusted = trustedNow;
        // SLN-4: MSBuild's model of each project configuration kept between sessions, per solution.
        if (!m_options.compileCommandsCache.empty())
            if (auto directory = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root) + "/project-model"))
                evaluation.cache = std::make_shared<project::ProjectModelCache>(m_services.fs, std::move(*directory));
        // SLN-5: clangd's commands from MSBuild's own GetClCommandLines (startClangd's provider).
        evaluation.clCommandLines = m_options.clangd;
        m_solutionEvaluation = std::make_shared<project::SolutionEvaluation>(m_services.launcher, m_services.fs,
                                                                             m_solution, std::move(evaluation));
    }
    /* The independent MSBuild.exe evaluation feeds clangd and the explorer only without ProjectHost's:
     * with it, clangd takes GetClCommandLines (SLN-5) and, should ProjectHost fail, the chain's own
     * MSBuild provider, so the solution's projects are not evaluated twice at once. */
    if (!m_solution || !admission.trusted || m_options.compileCommandsCache.empty() || m_solutionEvaluation)
        return;
    auto cache = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root));
    if (!cache)
        return;
    m_streamedProjects = std::make_shared<lang::StreamedProjectContexts>(m_commandsRetirement, m_solution);
    lang::MsBuildProviderOptions options;
    options.retirement = m_commandsRetirement;
    options.streamedProjects = m_streamedProjects;
    options.configuration = configuration;
    const auto settings = m_services.settings.snapshot();
    options.msbuildPath =
        !m_options.msbuildPath.empty() ? m_options.msbuildPath : settings->get(settings::key::toolsMsbuildPath);
    options.locateMsBuild = locateMsBuild;
    options.trusted = trustedNow;
    options.fallback.configuration = options.configuration;
    m_projectEvaluation = std::make_shared<lang::IndependentProjectEvaluation>(m_services.launcher, m_services.fs,
                                                                               m_solution, *cache, std::move(options));
}

std::shared_ptr<const project::SolutionModel> Workbench::explorerSolutionModel() {
    const auto trust = m_services.trust.snapshot(m_projectRoot);
    if (!m_solution || !trust.trusted || trust.revision != m_projectTrustRevision)
        return m_solution;
    if (m_solutionEvaluation) {
        // Re-composed only when the evaluation published something new or the static model was
        // reloaded (an edit, a file-system change); otherwise the explorer keeps its model.
        const auto status = m_solutionEvaluation->status();
        if (m_evaluatedFrom != m_solution || m_evaluatedGeneration != status.generation) {
            m_evaluatedFrom = m_solution;
            m_evaluatedGeneration = status.generation;
            m_evaluatedSolution.reset();
            if (auto results = m_solutionEvaluation->results(); !results.empty()) {
                auto loaded = project::SolutionModel::load(m_services.fs, m_solution->path(),
                                                           {.cppHierarchy = m_cppHierarchyMode,
                                                            .unloadedProjectPaths = m_unloadedProjectPaths,
                                                            .evaluated = std::move(results)});
                if (loaded)
                    m_evaluatedSolution = std::make_shared<const project::SolutionModel>(std::move(*loaded));
                else
                    log::warn(kLog, "solution evaluation: {}", loaded.error().describe());
            }
        }
        if (m_evaluatedSolution)
            return m_evaluatedSolution;
    }
    if (m_streamedProjects) {
        if (auto evaluated = m_streamedProjects->modelSnapshot())
            return evaluated;
    }
    return m_solution;
}

QString Workbench::solutionEvaluationBanner() const {
    if (!m_solutionEvaluation)
        return {};
    const auto status = m_solutionEvaluation->status();
    // The truth rule: say which evaluator the items come from when it is not ProjectHost.
    if (!status.msbuildProblem.empty())
        return text("The projects are not evaluated (%1). The Solution Explorer shows the project files as written; "
                    "items that property sheets or conditions add are missing.")
            .arg(qs(status.msbuildProblem));
    if (!status.projectHostProblem.empty())
        return text("ProjectHost is unavailable (%1). MSBuild.exe evaluates the projects instead.")
            .arg(qs(status.projectHostProblem));
    return {};
}

void Workbench::startClangd(const std::string& root) {
    if (m_shutDown)
        return;
    if (m_buildService->projectEditInProgress()) {
        m_projectEvaluationDeferred = true;
        return;
    }
    stopClangd();
    if (!m_options.clangd) {
        return;
    }
    const auto snapshot = m_services.settings.snapshot();
    Result<lang::ClangdLocation> found =
        lang::findClangd(m_services.fs, lang::defaultClangdSearch(lang::configuredClangdPath(*snapshot)));
    if (!found) {
        log::info(kLog, "no clangd, code insight is off: {}", found.error().describe());
        // The truth rule (ADR 0078): no C++ results without clangd, and the user is told why, once
        // per project (as for LuaLS), not only in the log.
        if (m_clangdUnavailableNotifiedRoot != root) {
            m_clangdUnavailableNotifiedRoot = root;
            notify(text("C++ language server unavailable"),
                   qs(std::format("{}. C++ diagnostics, completion, navigation and the Structure view are off until "
                                  "clangd {} is installed (tools/bootstrap) or Settings > Tools names a clangd "
                                  "executable.",
                                  found.error().message(), lang::pinnedLlvmVersion())),
                   ui::NotificationKind::Warning);
        }
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

    // ADR 0041's order: an existing compile_commands.json, then MSBuild's evaluation (a trusted
    // solution, Windows), then the commands synthesized from the projects.
    std::optional<std::string> cache;
    if (!m_options.compileCommandsCache.empty()) {
        if (Result<std::string> joined = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root)); joined) {
            cache = std::move(*joined);
        }
    }
    if (m_cmake) {
        m_clangdCMakeState = cmakeClangdState(m_services.fs, m_cmake->service());
        std::weak_ptr<build::CMakeService> service = m_cmake->sharedService();
        if (cache) {
            // CPP-3: one directory of the IDE's per configuration, never the build tree (shards).
            m_cmakeCommands = std::make_shared<lang::CMakeCompileCommandsProvider>(m_services.fs, *cache, [service] {
                const auto current = service.lock();
                return current ? cmakeCompileCommandsInput(*current) : lang::CMakeCompileCommandsInput{};
            });
            m_compileCommands = m_cmakeCommands;
        } else {
            m_compileCommands = std::make_shared<lang::BuildDirectoryCompileCommandsProvider>(m_services.fs, [service] {
                const auto current = service.lock();
                return current ? current->activeBuildDirectory() : std::string();
            });
        }
    } else if (m_standalone && m_standalone->model().database) {
        // An explicit standalone model pins clangd to the discovered database, rather than a
        // stale database in another conventional build directory. This provider rereads existence.
        const std::string directory(io::parentPath(*m_standalone->model().database));
        m_compileCommands = std::make_shared<lang::BuildDirectoryCompileCommandsProvider>(
            m_services.fs, [directory] { return directory; });
    } else if (m_solution && cache) {
        lang::MsBuildProviderOptions options;
        options.retirement = m_commandsRetirement;
        if (const std::optional<std::size_t> active = m_build->activeConfigurationIndex();
            active && *active < m_build->configurations().size()) {
            options.configuration = m_build->configurations()[*active].name();
        }
        options.msbuildPath =
            !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(settings::key::toolsMsbuildPath);
        options.locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
            Result<build::VsInstance> instance = build::findMsBuild(*launcher, *fs);
            if (!instance) {
                return std::unexpected(std::move(instance.error()));
            }
            if (instance->msbuildPath.empty()) {
                return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
            }
            return instance->msbuildPath;
        };
        const auto admission = m_services.trust.snapshot(root);
        options.trusted = [trust = &m_services.trust, root, revision = admission.revision] {
            const auto current = trust->snapshot(root);
            return current.trusted && current.revision == revision;
        };
        options.fallback.configuration = options.configuration;
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            options.msvc = [launcher = &m_services.launcher, fs = &m_services.fs] {
                return lang::findMsvcEnvironment(*launcher, *fs);
            };
        }
        // SLN-5: one owned database per solution configuration from GetClCommandLines, ahead of
        // the evaluated and synthesized ones (which still answer when ProjectHost is unavailable).
        std::shared_ptr<lang::ICompileCommandsProvider> clCommandLines;
        const std::string configurationName = options.configuration.empty() && !m_solution->configurations().empty()
                                                  ? m_solution->configurations().front().name()
                                                  : options.configuration;
        if (m_solutionEvaluation)
            if (auto directory =
                    io::joinPath(*cache, "clcommandlines/" + lang::configurationDirectoryName(configurationName)))
                clCommandLines = std::make_shared<lang::ClCommandLinesCompileCommandsProvider>(
                    m_services.fs, m_solutionEvaluation, std::move(*directory));
        m_compileCommands =
            lang::solutionCompileCommandsChain(m_services.launcher, m_services.fs, m_solution, root, *cache,
                                               std::move(options), m_projectEvaluation, std::move(clCommandLines));
    } else {
        m_compileCommands = std::make_shared<lang::ExistingCompileCommandsProvider>(m_services.fs, root);
    }

    if (!m_resourceGovernor) {
        m_resourceGovernor = std::make_shared<sched::ResourceGovernor>();
    }
    const bool buildActive = (m_buildService && m_buildService->state() == build::BuildState::Running) ||
                             (m_cmake && m_cmake->service().state() == build::BuildState::Running);
    m_resourceGovernor->setLegActive(sched::HeavyLegKind::Build, buildActive);

    lang::ClangdServiceOptions options;
    options.clangdPath = found->path;
    options.requireFrostPlatform = found->origin == lang::ClangdOrigin::BundledFrost;
    options.workspaceRoot = root;
    options.launch.jobs = m_options.clangdJobs;
    if (!m_options.clangdLogLevel.empty())
        options.launch.logLevel = m_options.clangdLogLevel;
    options.onLog = m_options.clangdLog;
    // Restricted mode (no project config, default limits) unless the root is trusted (clangd.hpp).
    options.launch.trusted = m_services.trust.isTrusted(root);
    m_clangdTrusted = options.launch.trusted;
    options.compileCommands = m_compileCommands;
    const auto generation = m_clangdGeneration;
    // The owned server supervisors are joined before Workbench's QObject base is destroyed;
    // this worker callback only posts, and the queued GUI callback rechecks the controller.
    options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation] {
                    if (!self || self->m_clangdGeneration != generation)
                        return;
                    if (self->m_engineBindings)
                        self->m_engineBindings->cancel();
                    if (self->m_language)
                        self->m_language->structureChanged();
                    // Reads the new server's database for languageReadiness before anyone asks, under
                    // the spelling clangdUsesFallbackFlags looks it up by (normalised <dir>/name).
                    if (self->m_clangd && self->m_clangd->state() == lang::ClangdState::Running)
                        if (const auto location = self->m_clangd->compileCommands()) {
                            auto joined = io::joinPath(location->directory, "compile_commands.json");
                            if (auto file = joined ? io::normalizePath(*joined) : joined)
                                (void)self->compileDatabaseState(*file);
                        }
                },
                Qt::QueuedConnection);
    };
    // Called on clangd's threads; the queued calls die with this object.
    options.onProgress = [this, generation](const lang::ClangdProgress& progress) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, progress] {
                if (generation == m_clangdGeneration)
                    onClangdProgress(progress);
            },
            Qt::QueuedConnection);
    };
    m_clangd = std::make_shared<lang::ClangdService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                     std::move(options));
    lang::setResourceGovernor(*m_clangd, m_resourceGovernor);
    m_clangdListener =
        m_clangd->diagnostics().addListener([this, generation](const lang::FileDiagnostics& diagnostics) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, file = diagnostics.file] {
                    if (generation == m_clangdGeneration)
                        onLiveDiagnostics(file);
                },
                Qt::QueuedConnection);
        });
    if (auto started = m_clangd->start(); !started) {
        notify(text("clangd cannot start"), qs(started.error().message()), ui::NotificationKind::Warning);
        stopClangd();
        return;
    }
    m_bridge->setServer(m_clangd);
    for (DocumentEditorView* view : m_editors->uniqueDocumentViews())
        syncClangdDocument(*view);
    for (DocumentEditorView* view : m_editors->views()) {
        if (isCFamilySource(view->path()) && view == currentView()) {
            auto* binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
            binding->setCodeVisionEnabled(true);
            binding->attach(*m_clangd);
        }
    }
    m_language->structureChanged();
    updateResolveContext();
}

void Workbench::restartClangd() {
    // A configuration switch: other flags, so another database (the chain regenerates it).
    if (hasProject()) {
        log::info(kLog, "the configuration or trust of the project changed: clangd restarts");
        startClangd(m_projectRoot);
    }
}

void Workbench::restartClangdIfCMakeStateChanged() {
    if (!m_cmake || !m_clangd)
        return;
    const std::string state = cmakeClangdState(m_services.fs, m_cmake->service());
    const bool trusted = m_services.trust.isTrusted(m_projectRoot);
    if (state == m_clangdCMakeState && trusted == m_clangdTrusted) {
        // Same build directory, database present or absent as before, same trust: clangd already
        // reads it with the right launch flags.
        log::info(kLog, "the CMake model changed; clangd's compile commands did not, so it keeps running");
        refreshOwnedCompileCommands(); // A reconfigure may have changed the commands in place.
        updateResolveContext();
        return;
    }
    /* R0 follow-up 10: a profile switch to a never-configured build directory starts its configure
     * before setActiveProfile returns, and its reply is read on the service thread afterwards
     * (modelLoading()). Restarting while either is pending would run clangd without a database
     * and once more when the configure writes it, so clangd keeps the old database until the read
     * and the configure end: the configure's publication restarts clangd once, and when it writes
     * no database the trust watch restarts it once the service is idle. modelLoading is read
     * first: the read may start the configure as it ends. A trust change does not wait (its launch
     * flags apply at once), and neither does a job held in NeedsTrust: it runs nothing until the
     * user answers, which may be never. */
    const bool loading = m_cmake->service().modelLoading();
    const build::BuildState job = m_cmake->service().state();
    if (trusted == m_clangdTrusted && !state.ends_with(kCMakeDatabasePresent) &&
        (loading || (job != build::BuildState::Idle && job != build::BuildState::NeedsTrust))) {
        log::info(kLog, "the CMake compile commands moved ({} -> {}); clangd waits for the running CMake job",
                  m_clangdCMakeState, state);
        return;
    }
    if (trusted != m_clangdTrusted)
        log::info(kLog, "the project's trust changed ({} -> {}): clangd restarts with the new launch flags",
                  m_clangdTrusted, trusted);
    else
        log::info(kLog, "the CMake compile commands moved ({} -> {}): clangd restarts", m_clangdCMakeState, state);
    restartClangd();
}

void Workbench::refreshOwnedCompileCommands() {
    if (!m_cmakeCommands || !m_clangd)
        return;
    std::erase_if(m_retiredClangd, [](auto& task) {
        if (!task.ready())
            return false;
        task.join();
        return true;
    });
    m_retiredClangd.push_back(m_pool->spawn(sched::Lane::Background, refreshOwnedCommands(m_cmakeCommands)));
}

void Workbench::attachResolveContext() {
    if (!m_window)
        return;
    auto* button = new ui::lang::ResolveContextButton(m_window->statusBar());
    m_window->statusBar()->addPermanentWidget(button);
    m_resolveContext = button;
    connect(
        button, &ui::lang::ResolveContextButton::contextChosen, this, [this, button](const QString& file, int index) {
            const QStringList targets = button->targets();
            chooseResolveContext(file.toStdString(),
                                 index >= 0 && index < targets.size() ? targets[index].toStdString() : std::string());
        });
}

void Workbench::updateResolveContext() {
    auto* button = qobject_cast<ui::lang::ResolveContextButton*>(m_resolveContext.data());
    if (button == nullptr)
        return;
    DocumentEditorView* view = currentView();
    if (!view || !m_cmake || !m_cmakeCommands || !m_clangd || !isCFamilySource(view->path())) {
        button->hideContexts();
        return;
    }
    const lang::CMakeCompileCommandsInput input = cmakeCompileCommandsInput(m_cmake->service());
    if (!input.model || !lang::cmakeModelDescribes(*input.model, input.buildDirectory, input.configuration)) {
        button->hideContexts();
        return;
    }
    // A file only one target lists has one context; the selector is for shared headers and sources.
    const std::vector<std::size_t> owners = project::cmakeTargetsListing(*input.model, view->path());
    if (owners.size() < 2) {
        button->hideContexts();
        return;
    }
    const std::string chosen = m_cmakeCommands->context(view->path());
    QStringList names;
    int current = -1;
    for (const std::size_t owner : owners) {
        const std::string& name = input.model->targets[owner].name;
        if (name == chosen)
            current = static_cast<int>(names.size());
        names.push_back(qs(name));
    }
    button->showContexts(qs(view->path()), names, current);
}

void Workbench::chooseResolveContext(const std::string& file, const std::string& target) {
    if (!m_cmake || !m_cmakeCommands)
        return;
    if (auto saved = m_cmakeCommands->setContext(file, target); !saved)
        log::warn(kLog, "the resolve context of {} was not saved: {}", file, saved.error().describe());
    log::info(kLog, "resolve context of {}: {}", file, target.empty() ? std::string("automatic") : target);
    const auto model = m_cmake->service().model();
    const auto command = model && !target.empty() ? m_cmakeCommands->contextCommand(*model, file) : std::nullopt;
    /* A running clangd takes a chosen command at once (compilationDatabaseChanges reparses the open
     * file) and the owned database keeps it for the background index and later starts. clangd's
     * overlay cannot forget a command, so Automatic, or a choice clangd could not take, restarts
     * it on the rewritten database (its start rewrites it before launching). */
    if (command && m_clangd &&
        m_clangd->notify("workspace/didChangeConfiguration", lang::compilationDatabaseChange(*command))) {
        refreshOwnedCompileCommands();
        return;
    }
    restartClangd();
}

std::string Workbench::clangdCMakeStateNow() const {
    return m_cmake ? cmakeClangdState(m_services.fs, m_cmake->service()) : std::string();
}

void Workbench::resumeDeferredClangd(bool retryUntilIdle) {
    if (m_clangdDeferredRoot.empty())
        return;
    if (m_shutDown || !m_cmake || m_clangdDeferredRoot != m_projectRoot) {
        m_clangdDeferredRoot.clear();
        QObject::disconnect(m_clangdDeferral);
        return;
    }
    if (m_cmake->service().state() != build::BuildState::Idle) {
        // The controller can report Idle (or the model) just before the service's own state does.
        if (retryUntilIdle)
            QTimer::singleShot(std::chrono::milliseconds(50), this, [this] { resumeDeferredClangd(true); });
        return;
    }
    const std::string root = std::exchange(m_clangdDeferredRoot, {});
    QObject::disconnect(m_clangdDeferral);
    log::info(kLog, "the CMake configure of {} ended: clangd starts", root);
    startClangd(root);
}

void Workbench::stopClangd() {
    if (m_engineBindings)
        m_engineBindings->cancel();
    m_clangdDeferredRoot.clear();
    QObject::disconnect(m_clangdDeferral);
    m_clangdCMakeState.clear();
    m_clangdTrusted = false;
    ++m_clangdGeneration;
    std::erase_if(m_retiredClangd, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
    if (!m_clangd) {
        return;
    }
    m_language->structureChanged();
    for (DocumentEditorView* view : m_editors->views()) {
        if (isCFamilySource(view->path()))
            delete view->editor().findChild<ui::lang::EditorInsightBinding*>();
    }
    m_bridge->setServer(nullptr);
    m_clangd->diagnostics().removeListener(m_clangdListener);
    m_clangdListener = 0;
    auto old = std::move(m_clangd);
    auto stopped = old->stop(); // Signal even when the only pool worker is inside evaluation.
    m_retiredClangd.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedClangd(std::move(old), std::move(stopped))));
    m_compileCommands.reset();
    m_cmakeCommands.reset();
    updateResolveContext();
    if (m_indexing) {
        m_indexing->finish();
        m_indexing.reset();
    }
    m_indexToken.clear();
    m_clangdIndexing = false;
    if (m_resourceGovernor)
        m_resourceGovernor->setLegActive(sched::HeavyLegKind::BackgroundIndex, false);
    for (DocumentEditorView* view : m_editors->views()) {
        if (!isCFamilySource(view->path()))
            continue;
        m_liveMarkers.erase(lang::documentKey(view->path()));
        // Without clangd the editors show no live analysis: no squiggles, usages or inspection widget.
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setUsageHighlights({});
        const auto version = view->editor().editor().text().version();
        (void)view->editor().setSemanticTokens(version, {});
        (void)view->editor().setInactiveRanges(version, {});
        view->editor().setInsightEnabled(false);
        refreshMarkers(*view);
    }
    refreshLiveProblems();
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
            }
        }
        if (markers.empty()) {
            m_liveMarkers.erase(key);
        } else {
            m_liveMarkers[key] = std::move(markers);
        }
        for (DocumentEditorView* view : m_editors->views()) {
            if (lang::documentKey(view->path()) != key)
                continue;
            // The squiggles: every live diagnostic (editor-insight's translation; ranges of the
            // published text version, mapped by the view through the edits made since).
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, set ? ui::lang::toEditorDiagnostics(*set)
                                                                                : std::vector<ui::EditorDiagnostic>{});
            refreshMarkers(*view);
        }
    }
    refreshLiveProblems();
}

void Workbench::refreshLiveProblems() {
    if (!m_problems) {
        return;
    }
    std::vector<build::BuildDiagnostic> rows;
    std::vector<lang::LiveDiagnostic> live;
    if (m_clangd)
        live = m_clangd->diagnostics().problems();
    if (m_lua) {
        auto lua = m_lua->diagnostics().problems();
        live.insert(live.end(), std::make_move_iterator(lua.begin()), std::make_move_iterator(lua.end()));
    }
    for (lang::FileDiagnostics& set : luaInspectedDiagnostics()) {
        if (m_lua && m_lua->diagnostics().forFile(set.file))
            continue;
        live.insert(live.end(), std::make_move_iterator(set.diagnostics.begin()),
                    std::make_move_iterator(set.diagnostics.end()));
    }
    if (m_managed) {
        auto managed = m_managed->diagnostics().problems();
        live.insert(live.end(), std::make_move_iterator(managed.begin()), std::make_move_iterator(managed.end()));
    }
    if (m_bridge)
        for (const auto* view : m_editors->uniqueDocumentViews()) {
            auto json = m_bridge->jsonDiagnostics(view->path());
            live.insert(live.end(), std::make_move_iterator(json.begin()), std::make_move_iterator(json.end()));
        }
    rows.reserve(live.size());
    for (lang::LiveDiagnostic& d : live) {
        build::BuildDiagnostic row;
        row.diagnostic.severity = d.severity == lang::DiagnosticSeverity::Error     ? build::Severity::Error
                                  : d.severity == lang::DiagnosticSeverity::Warning ? build::Severity::Warning
                                                                                    : build::Severity::Message;
        row.diagnostic.code = std::move(d.code);
        row.diagnostic.message = std::move(d.message);
        row.diagnostic.file = d.file;
        row.diagnostic.line = d.line;
        row.diagnostic.column = d.column;
        row.jumpable = !d.file.empty() && d.line > 0;
        row.path = std::move(d.file);
        rows.push_back(std::move(row));
    }
    m_problems->model()->setProblems(ui::DiagnosticSource::Live, std::move(rows));
}

void Workbench::onClangdProgress(const lang::ClangdProgress& progress) {
    if (!m_clangd) {
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
            m_indexing.reset();
        }
        m_indexToken = progress.token;
        m_clangdIndexing = true; // languageReadiness, with or without a window.
        if (m_window)
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
        m_clangdIndexing = false;
        return;
    }
}

} // namespace frost::app
