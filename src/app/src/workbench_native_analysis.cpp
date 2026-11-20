// SPDX-License-Identifier: Apache-2.0
// RUN-12 actual selected-configuration coverage, original run preparation and independent gutters.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/run/native_analysis.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "cmake_integration.hpp"
#include "trusted_project_launcher.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <tuple>
namespace frost::app {
namespace {
QString text(std::string_view bytes) { return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())); }
using Cancel = std::stop_callback<std::function<void()>>;
using CoverageDone = std::function<void(Result<run::CoverageReport>)>;
sched::Task<void> coverageTask(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                               std::string root, std::uint64_t revision, run::CoverageRequest request,
                               std::stop_token stop, CoverageDone done) {
    TrustedProjectLauncher admitted(*launcher, *trust, root, revision, stop);
    auto permission = [=]() -> Result<void> {
        const auto current = trust->snapshot(root);
        if (stop.stop_requested() || !current.trusted || current.revision != revision)
            return fail(ErrorCode::Cancelled, "Coverage admission changed");
        return {};
    };
    done(run::collectNativeCoverage(*fs, admitted, request, stop, permission));
    co_return;
}
sched::Task<void> profileTask(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                              std::string root, std::uint64_t revision, run::NativeProfileRequest request,
                              std::stop_token stop, std::function<void(Result<run::NativeProfileReport>)> done) {
    auto permission = [=]() -> Result<void> {
        const auto current = trust->snapshot(root);
        if (stop.stop_requested() || !current.trusted || current.revision != revision)
            return fail(ErrorCode::Cancelled, "Profiling admission changed");
        return {};
    };
    // collectNativeProfile separately retains the installed recorder's mandatory OS-session cleanup authority.
    done(run::collectNativeProfile(*fs, *launcher, request, stop, permission));
    co_return;
}
sched::Task<void> admitCoverageSource(io::IFileSystem* fs, run::AnalysisFileIdentity binary,
                                      run::AnalysisFileIdentity source, std::stop_token stop,
                                      std::function<void(bool)> done) {
    const bool valid = !stop.stop_requested() && run::validateAnalysisIdentity(*fs, binary) &&
                       run::validateAnalysisIdentity(*fs, source);
    done(valid && !stop.stop_requested());
    co_return;
}
} // namespace
void Workbench::prepareNativeAnalysis(const run::RunConfiguration& configuration, std::stop_token stop,
                                      std::function<void(Result<run::LaunchPlan>)> done) {
    const QPointer<Workbench> self(this);
    const auto root = m_projectRoot;
    const auto revision = m_services.trust.snapshot(root).revision;
    auto tasks = [self, configuration, stop, done, root, revision](Result<run::LaunchPlan> plan) mutable {
        if (!self || stop.stop_requested()) {
            done(fail(ErrorCode::Cancelled, "Analysis preparation cancelled"));
            return;
        }
        if (!plan) {
            done(std::unexpected(plan.error()));
            return;
        }
        auto prepared = configuration;
        prepared.type = run::RunType::Native;
        prepared.target.clear();
        prepared.beforeLaunch =
            configuration.type == run::RunType::Native ? configuration.beforeLaunch : run::BeforeLaunch::None;
        prepared.program = plan->program;
        prepared.arguments = plan->arguments;
        prepared.workingDirectory = plan->workingDirectory;
        prepared.environment = plan->environment;
        const auto currentTrust = self->m_services.trust.snapshot(root);
        if (self->m_projectRoot != root || !currentTrust.trusted || currentTrust.revision != revision) {
            done(fail(ErrorCode::Cancelled, "Analysis preparation trust changed"));
            return;
        }
        auto cancel = std::make_shared<std::unique_ptr<Cancel>>();
        run::RunHandlers handlers;
        handlers.launchAdmission = [trust = &self->m_services.trust, root, revision, stop]() -> Result<void> {
            const auto current = trust->snapshot(root);
            if (stop.stop_requested() || !current.trusted || current.revision != revision)
                return fail(ErrorCode::Cancelled, "Analysis task admission changed");
            return {};
        };
        handlers.onFinished = [self, done, cancel](const run::RunResult& result) {
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, done, cancel, result] {
                        cancel->reset();
                        if (!self)
                            return;
                        if (result.error)
                            done(std::unexpected(*result.error));
                        else if (result.stopped || !result.plan)
                            done(fail(ErrorCode::Cancelled, "Analysis preparation stopped"));
                        else
                            done(*result.plan);
                    },
                    Qt::QueuedConnection);
        };
        auto started = self->m_runManager->start(prepared, std::move(handlers), run::RunMode::Prepare);
        if (!started) {
            done(std::unexpected(started.error()));
            return;
        }
        *cancel =
            std::make_unique<Cancel>(stop, [manager = self->m_runManager.get(), id = *started] { manager->stop(id); });
    };
    if (configuration.type == run::RunType::Native) {
        tasks(run::launchPlanFor(configuration));
        return;
    }
    auto targetReady = [tasks, configuration](Result<debug::DebugTarget> target) mutable {
        if (!target) {
            tasks(std::unexpected(target.error()));
            return;
        }
        run::LaunchPlan plan{target->program, target->arguments, target->workingDirectory, {}};
        for (const auto& [name, value] : target->environment)
            plan.environment.push_back({name, value});
        for (const auto& entry : configuration.environment) {
            std::erase_if(plan.environment, [&](const auto& current) { return current.name == entry.name; });
            plan.environment.push_back(entry);
        }
        tasks(std::move(plan));
    };
    if (configuration.type == run::RunType::CMakeTarget && m_cmake) {
        auto cancellation = std::make_shared<std::unique_ptr<Cancel>>();
        const QPointer<CMakeIntegration> project(m_cmake.get());
        auto accepted = m_cmake->debug(configuration, [targetReady, cancellation](auto result) mutable {
            cancellation->reset();
            targetReady(std::move(result));
        });
        if (!accepted) {
            done(std::unexpected(accepted.error()));
            return;
        }
        *cancellation = std::make_unique<Cancel>(stop, [project] {
            if (project)
                QMetaObject::invokeMethod(
                    project.data(),
                    [project] {
                        if (project)
                            project->stop();
                    },
                    Qt::QueuedConnection);
        });
        return;
    }
    if (configuration.type == run::RunType::MSBuild && m_solution && m_runService) {
        const auto project = m_solution->findProjectByPath(configuration.target);
        if (!project) {
            done(fail(ErrorCode::NotFound, "Analysis project no longer exists"));
            return;
        }
        auto cancellation = std::make_shared<std::unique_ptr<Cancel>>();
        build::DebugRunHandlers handlers;
        handlers.onFinished = [self, targetReady, configuration, cancellation](
                                  const build::RunOutcome& outcome, const std::optional<build::DebugLaunch>& launch) {
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, targetReady, configuration, cancellation, outcome, launch]() mutable {
                        cancellation->reset();
                        if (!self)
                            return;
                        if (outcome.error || !launch) {
                            targetReady(outcome.error ? Result<debug::DebugTarget>(std::unexpected(*outcome.error))
                                                      : Result<debug::DebugTarget>(fail(
                                                            ErrorCode::Cancelled, "Analysis project build stopped")));
                            return;
                        }
                        auto target = ui::debugTargetFor(outcome, *launch);
                        if (!configuration.program.empty())
                            target.program = configuration.program;
                        if (!configuration.arguments.empty())
                            target.arguments = configuration.arguments;
                        if (!configuration.workingDirectory.empty())
                            target.workingDirectory = configuration.workingDirectory;
                        targetReady(std::move(target));
                    },
                    Qt::QueuedConnection);
        };
        auto accepted = m_runService->debug(*project, std::move(handlers));
        if (!accepted) {
            done(std::unexpected(accepted.error()));
            return;
        }
        *cancellation =
            std::make_unique<Cancel>(stop, [service = m_runService.get(), id = *accepted] { service->stop(id); });
        return;
    }
    done(fail(ErrorCode::Unsupported, "Coverage requires a native, CMake executable or MSBuild C++ run configuration"));
}
void Workbench::registerNativeAnalysisActions() {
    for (const auto& [id, label, import, profile] :
         std::array{std::tuple{"run.coverage", "Run Selected Configuration with LLVM Coverage", false, false},
                    std::tuple{"run.importCoverage", "Prepare Selected Configuration and Import LLVM Profiles...", true,
                               false},
                    std::tuple{"run.profile", "Profile Selected Native Configuration...", false, true}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = label;
        action.group = "Run";
        action.executesProjectCode = true;
        action.enabled = [this](const auto&) {
            return !m_shutDown && !m_nativeAnalysisActive && !m_projectRoot.empty();
        };
        action.handler = [this, import, profile](const auto&, const auto&) { startNativeAnalysis(import, profile); };
        (void)m_registry.add(std::move(action));
    }
    input::ActionSpec stop;
    stop.id = "run.stopNativeAnalysis";
    stop.name = "Stop Native Coverage or Profiling";
    stop.group = "Run";
    stop.handler = [this](const auto&, const auto&) { m_nativeAnalysisStop.request_stop(); };
    (void)m_registry.add(std::move(stop));
    connect(this, &Workbench::projectChanged, this, [this] {
        m_nativeAnalysisStop.request_stop();
        ++m_nativeAnalysisGeneration;
        m_nativeAnalysisActive = false;
    });
}
void Workbench::startNativeAnalysis(bool importProfiles, bool profile) {
    if (m_shutDown || m_nativeAnalysisActive || !m_runController || !m_runManager || !m_pool || !m_window)
        return;
    const auto* selected = m_runController->selectedConfiguration();
    if (!selected)
        return;
    const auto configuration = *selected;
    const auto root = m_projectRoot;
    if (configuration.type != run::RunType::Native && configuration.type != run::RunType::CMakeTarget &&
        configuration.type != run::RunType::MSBuild) {
        QMessageBox::information(m_window, "Coverage", "Choose a native C++ executable configuration.");
        return;
    }
    if (m_runController->running() || (m_debugSessions && m_debugSessions->anyDebugging()) ||
        m_buildService->projectEditInProgress())
        return;
    if (auto allowed = mayRunConfiguration(
            configuration, [this, importProfiles, profile] { startNativeAnalysis(importProfiles, profile); });
        !allowed)
        return;
    const auto revision = m_services.trust.snapshot(root).revision;
    const QPointer<Workbench> self(this);
    QStringList profiles;
    if (importProfiles) {
        profiles = QFileDialog::getOpenFileNames(m_window, "Select actual LLVM raw profiles", {},
                                                 "LLVM raw profile (*.profraw)");
        if (!self || profiles.empty())
            return;
    }
    int seconds = 10;
    std::string profiler;
    if (profile) {
        bool accepted = false;
        seconds = QInputDialog::getInt(
            m_window, "Native CPU profiling",
            "Sampling interval in seconds (the owned program tree ends at this interval):", 10, 1, 300, 1, &accepted);
        if (!self || !accepted)
            return;
#ifdef _WIN32
        if (QMessageBox::question(m_window, "Windows ETW sampling",
                                  "Record system-wide CPU/process/module activity locally using an owned WPR instance? "
                                  "Other processes may appear in the actual report.") != QMessageBox::Yes ||
            !self)
            return;
        auto installed = io::joinPath(qEnvironmentVariable("ProgramFiles(x86)").toStdString(),
                                      "Windows Kits/10/Windows Performance Toolkit/xperf.exe");
        if (installed && m_services.fs.exists(*installed).value_or(false))
            profiler = *installed;
#else
        profiler = "/usr/bin/perf";
#endif
        if (profiler.empty() || !m_services.fs.exists(profiler).value_or(false)) {
            profiler =
                QFileDialog::getOpenFileName(m_window, "Select installed native profiler report tool").toStdString();
            if (!self || profiler.empty())
                return;
        }
    }
    const auto parent = QFileDialog::getExistingDirectory(m_window, "Choose local analysis artifact directory");
    if (!self || parent.isEmpty())
        return;
    const auto current = m_runController->selectedConfiguration();
    if (m_shutDown || m_projectRoot != root || !current || *current != configuration ||
        !m_services.trust.isTrusted(root) || m_services.trust.snapshot(root).revision != revision)
        return;
    m_nativeAnalysisStop.request_stop();
    m_nativeAnalysisStop = std::stop_source{};
    const auto token = m_nativeAnalysisStop.get_token();
    const auto generation = ++m_nativeAnalysisGeneration;
    m_nativeAnalysisActive = true;
    const auto solution = m_solution;
    const auto active = m_build->activeConfigurationIndex();
    const auto cmake = m_cmake.get();
    const auto model = m_cmake ? m_cmake->shownModel() : nullptr;
    const auto contextCurrent = [self, root, revision, generation, configuration, token, solution, active, cmake,
                                 model] {
        if (!self || self->m_shutDown || token.stop_requested() || self->m_projectRoot != root ||
            self->m_nativeAnalysisGeneration != generation)
            return false;
        if (self->m_solution != solution || self->m_build->activeConfigurationIndex() != active ||
            self->m_cmake.get() != cmake || (self->m_cmake && self->m_cmake->shownModel() != model))
            return false;
        const auto trust = self->m_services.trust.snapshot(root);
        const auto current = self->m_runController->selectedConfiguration();
        return trust.trusted && trust.revision == revision && current && *current == configuration;
    };
    auto* progress =
        new QProgressDialog("Preparing selected configuration and actual coverage tools...", "Stop", 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->show();
    connect(progress, &QProgressDialog::canceled, this,
            [source = m_nativeAnalysisStop]() mutable { source.request_stop(); });
    const QPointer<QProgressDialog> dialog(progress);
    const auto finished = [self, dialog, generation, contextCurrent,
                           token](Result<run::CoverageReport> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, dialog, generation, contextCurrent, token, result = std::move(result)]() mutable {
                if (dialog) {
                    QObject::disconnect(dialog.data(), nullptr, self.data(), nullptr);
                    dialog->close();
                }
                if (!self)
                    return;
                if (self->m_nativeAnalysisGeneration == generation)
                    self->m_nativeAnalysisActive = false;
                if (!contextCurrent())
                    return;
                if (!result) {
                    QMessageBox::warning(self->m_window, "Coverage", text(result.error().message()));
                    return;
                }
                auto report = std::make_shared<run::CoverageReport>(std::move(*result));
                auto* output = new QDialog(self->m_window);
                output->setAttribute(Qt::WA_DeleteOnClose);
                output->setObjectName("nativeCoverageResults");
                output->setWindowTitle("Actual LLVM coverage snapshot");
                auto* layout = new QVBoxLayout(output);
                layout->addWidget(
                    new QLabel("Counts come from llvm-cov for the selected binary. Source hashes describe observed "
                               "saved snapshots; they do not prove an arbitrary binary was built from those bytes.",
                               output));
                auto* table = new QTableWidget(static_cast<int>(report->files.size()), 4, output);
                table->setObjectName("nativeCoverageFiles");
                table->setHorizontalHeaderLabels(
                    {"Source", "Covered / mapped lines", "Covered / mapped branches", "Source snapshot"});
                table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                for (std::size_t row = 0; row < report->files.size(); ++row) {
                    const auto& file = report->files[row];
                    table->setItem(static_cast<int>(row), 0, new QTableWidgetItem(text(file.path)));
                    table->setItem(static_cast<int>(row), 1,
                                   new QTableWidgetItem(QStringLiteral("%1 / %2").arg(file.covered).arg(file.found)));
                    table->setItem(static_cast<int>(row), 2,
                                   new QTableWidgetItem(
                                       QStringLiteral("%1 / %2").arg(file.branchesCovered).arg(file.branchesFound)));
                    table->setItem(static_cast<int>(row), 3,
                                   new QTableWidgetItem(file.identity ? "Observed, not build provenance"
                                                                      : text(file.sourceUnavailable)));
                }
                layout->addWidget(table);
                auto* gutter = new QPushButton("Show actual counts in selected source gutter", output);
                gutter->setObjectName("nativeCoverageApplyGutter");
                layout->addWidget(gutter);
                const QPointer<QDialog> owner(output);
                connect(gutter, &QPushButton::clicked, output, [self, owner, table, report, contextCurrent, token] {
                    const int row = table->currentRow();
                    if (!contextCurrent() || row < 0 || static_cast<std::size_t>(row) >= report->files.size())
                        return;
                    const auto file = report->files[static_cast<std::size_t>(row)];
                    if (!file.identity)
                        return;
                    auto done = [self, owner, file, contextCurrent](bool valid) {
                        if (self)
                            QMetaObject::invokeMethod(
                                self.data(),
                                [self, owner, file, contextCurrent, valid] {
                                    if (!owner || !valid || !contextCurrent())
                                        return;
                                    if (!self->openPath(file.path))
                                        return;
                                    auto* view = self->currentView();
                                    if (!view || view->document().dirty() ||
                                        io::sha256Hex(view->document().document().text().text()) !=
                                            file.identity->sha256)
                                        return;
                                    std::vector<ui::CoverageMarker> markers;
                                    for (const auto& line : file.lines)
                                        markers.push_back({LineIndex{line.line - 1}, line.hits});
                                    view->editor().setCoverageMarkers(std::move(markers));
                                },
                                Qt::QueuedConnection);
                    };
                    self->m_pool
                        ->spawn(sched::Lane::Background, admitCoverageSource(&self->m_services.fs, report->binary,
                                                                             *file.identity, token, std::move(done)))
                        .detach();
                });
                auto* close = new QDialogButtonBox(QDialogButtonBox::Close, output);
                connect(close, &QDialogButtonBox::rejected, output, &QDialog::reject);
                layout->addWidget(close);
                output->resize(1050, 580);
                output->show();
            },
            Qt::QueuedConnection);
    };
    const auto finishedProfile = [self, dialog, generation,
                                  contextCurrent](Result<run::NativeProfileReport> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, dialog, generation, contextCurrent, result = std::move(result)]() mutable {
                if (dialog) {
                    QObject::disconnect(dialog.data(), nullptr, self.data(), nullptr);
                    dialog->close();
                }
                if (!self)
                    return;
                if (self->m_nativeAnalysisGeneration == generation)
                    self->m_nativeAnalysisActive = false;
                if (!contextCurrent())
                    return;
                if (!result) {
                    QMessageBox::warning(self->m_window, "Native profiling", text(result.error().message()));
                    return;
                }
                auto* output = new QDialog(self->m_window);
                output->setAttribute(Qt::WA_DeleteOnClose);
                output->setObjectName("nativeProfileResults");
                output->setWindowTitle("Actual native CPU sample report");
                auto* layout = new QVBoxLayout(output);
                const QString scope =
                    result->backend == run::NativeProfileBackend::WindowsEtw
                        ? QStringLiteral("System-wide ETW samples. Owned target PID: %1. Local symbol resolution only; "
                                         "unresolved entries remain unresolved.")
                              .arg(result->programPid ? QString::number(*result->programPid)
                                                      : QStringLiteral("unavailable"))
                        : QStringLiteral("perf samples for the selected command and inherited children. Actual report "
                                         "PIDs and unresolved symbols are preserved.");
                layout->addWidget(new QLabel(scope, output));
                layout->addWidget(new QLabel(text(result->trace.path), output));
                auto* search = new QLineEdit(output);
                search->setPlaceholderText("Find function/module/process in actual tool output");
                layout->addWidget(search);
                auto* report = new QPlainTextEdit(output);
                report->setObjectName("nativeProfileToolReport");
                report->setReadOnly(true);
                report->setPlainText(text(result->toolReport));
                layout->addWidget(report);
                connect(search, &QLineEdit::returnPressed, report, [search, report] { report->find(search->text()); });
                if (!result->diagnostics.empty()) {
                    auto* detail = new QPlainTextEdit(output);
                    detail->setReadOnly(true);
                    detail->setPlainText(text(result->diagnostics));
                    layout->addWidget(detail);
                }
                auto* close = new QDialogButtonBox(QDialogButtonBox::Close, output);
                connect(close, &QDialogButtonBox::rejected, output, &QDialog::reject);
                layout->addWidget(close);
                output->resize(1100, 700);
                output->show();
            },
            Qt::QueuedConnection);
    };
    saveAll([self, contextCurrent, configuration, token, finished, finishedProfile, root, revision, parent, profiles,
             importProfiles, profile, seconds, profiler](Result<void> saved) mutable {
        if (!self)
            return;
        if (!saved) {
            finished(std::unexpected(saved.error()));
            return;
        }
        if (!contextCurrent()) {
            finished(fail(ErrorCode::Cancelled, "Coverage preparation context changed"));
            return;
        }
        self->prepareNativeAnalysis(
            configuration, token,
            [self, contextCurrent, token, finished, finishedProfile, root, revision, parent, profiles, importProfiles,
             profile, seconds, profiler](Result<run::LaunchPlan> plan) mutable {
                if (!self)
                    return;
                if (!plan) {
                    finished(std::unexpected(plan.error()));
                    return;
                }
                if (!contextCurrent()) {
                    finished(fail(ErrorCode::Cancelled, "Coverage plan context changed"));
                    return;
                }
                if (profile) {
                    run::NativeProfileRequest request;
                    request.plan = std::move(*plan);
                    request.duration = std::chrono::seconds(seconds);
                    request.sessionName =
                        "FrostIDE-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                    request.outputDirectory =
                        io::joinPath(parent.toStdString(),
                                     "profile-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString())
                            .value_or("");
#ifdef _WIN32
                    request.backend = run::NativeProfileBackend::WindowsEtw;
                    request.systemWideEtwConsent = true;
                    request.xperf = profiler;
                    const auto system = build::systemCommandProcessor();
                    if (!system) {
                        finishedProfile(std::unexpected(system.error()));
                        return;
                    }
                    request.wpr = io::joinPath(io::parentPath(*system), "wpr.exe").value_or("");
#else
                    request.backend = run::NativeProfileBackend::Perf;
                    request.perf = profiler;
#endif
                    self->m_pool
                        ->spawn(sched::Lane::Background,
                                profileTask(&self->m_services.fs, &self->m_services.launcher, &self->m_services.trust,
                                            root, revision, std::move(request), token, finishedProfile))
                        .detach();
                    return;
                }
                run::CoverageRequest request;
                request.plan = std::move(*plan);
                request.compilationDirectory = root;
                request.execute = !importProfiles;
                request.outputDirectory =
                    io::joinPath(parent.toStdString(),
                                 "coverage-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString())
                        .value_or("");
                for (const auto& profile : profiles)
                    request.rawProfiles.push_back(profile.toStdString());
                for (const auto& directory : lang::defaultClangdSearch().sdkDirectories) {
                    if (request.llvmCov.empty()) {
                        const auto path = io::joinPath(directory,
#ifdef _WIN32
                                                       "llvm-cov.exe"
#else
                                                       "llvm-cov"
#endif
                        );
                        if (path && self->m_services.fs.exists(*path).value_or(false))
                            request.llvmCov = *path;
                    }
                    if (request.llvmProfdata.empty()) {
                        const auto path = io::joinPath(directory,
#ifdef _WIN32
                                                       "llvm-profdata.exe"
#else
                                                       "llvm-profdata"
#endif
                        );
                        if (path && self->m_services.fs.exists(*path).value_or(false))
                            request.llvmProfdata = *path;
                    }
                }
                if (request.llvmCov.empty() || request.llvmProfdata.empty()) {
                    finished(fail(ErrorCode::NotFound, "Pinned llvm-cov/llvm-profdata tools unavailable"));
                    return;
                }
                self->m_pool
                    ->spawn(sched::Lane::Background,
                            coverageTask(&self->m_services.fs, &self->m_services.launcher, &self->m_services.trust,
                                         root, revision, std::move(request), token, finished))
                    .detach();
            });
    });
}
} // namespace frost::app
