// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): asynchronous C++ Inspect Code, Problems publication and
// explicit SARIF export (CPP-10). The worker runs only the real clang-tidy through IProcessLauncher.

#include "frost/app/workbench.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/cmake_compile_commands.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "cmake_integration.hpp"
#include "trusted_project_launcher.hpp"
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <stop_token>
#include <utility>

namespace frost::app {
namespace {

using InspectionDone = std::function<void(Result<lang::InspectionReport>)>;

sched::Task<void> inspectCppTask(io::IFileSystem* fs, io::IProcessLauncher* launcher, project::TrustStore* trust,
                                 std::uint64_t trustRevision, std::shared_ptr<lang::ICompileCommandsProvider> commands,
                                 std::shared_ptr<sched::ResourceGovernor> governor, std::string projectRoot,
                                 std::string scopeDirectory, lang::ClangdSearch search,
                                 std::vector<std::string> plugins, std::stop_token stop, InspectionDone done) {
    TrustedProjectLauncher admitted(*launcher, *trust, projectRoot, trustRevision, stop);
    if (stop.stop_requested()) {
        done(fail(ErrorCode::Cancelled, "C++ inspection was cancelled"));
        co_return;
    }
    auto located = commands ? commands->locate(stop) : lang::findCompileCommands(*fs, projectRoot);
    if (!located) {
        done(std::unexpected(located.error()));
        co_return;
    }
    if (!*located) {
        done(fail(ErrorCode::NotFound,
                  "C++ inspection is unavailable: no compile_commands.json is available for this project"));
        co_return;
    }

    auto tool = lang::findClangTidy(*fs, admitted, search);
    if (!tool) {
        done(std::unexpected(tool.error()));
        co_return;
    }

    lang::InspectionRequest request;
    request.clangTidyPath = tool->path;
    request.compileCommandsDir = (*located)->directory;
    request.scope = lang::InspectionScope{
        .kind = lang::InspectionScopeKind::Directory, .paths = {std::move(scopeDirectory)}, .targetName = {}};
    request.profile.name = "Project";
    request.profile.respectProjectClangTidy = true;
    request.governor = governor.get();
    request.stopToken = stop;
    request.extraArgs.reserve(plugins.size());
    for (std::string& plugin : plugins) {
        if (!plugin.empty())
            request.extraArgs.push_back("--load=" + std::move(plugin));
    }
    done(lang::runClangTidyInspection(admitted, *fs, request));
}

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

build::Severity problemSeverity(lang::InspectionSeverity severity) noexcept {
    switch (severity) {
    case lang::InspectionSeverity::Error: return build::Severity::Error;
    case lang::InspectionSeverity::Warning:
    case lang::InspectionSeverity::WeakWarning: return build::Severity::Warning;
    case lang::InspectionSeverity::Info:
    case lang::InspectionSeverity::Disabled: return build::Severity::Message;
    }
    return build::Severity::Message;
}

bool isPathWithin(std::string_view root, std::string_view path) {
    const std::string rootKey = lang::documentKey(root);
    const std::string pathKey = lang::documentKey(path);
    if (pathKey == rootKey)
        return true;
    if (!rootKey.empty() && (rootKey.back() == '/' || rootKey.back() == '\\'))
        return pathKey.starts_with(rootKey);
    if (pathKey.size() <= rootKey.size() || !pathKey.starts_with(rootKey))
        return false;
    const char boundary = pathKey[rootKey.size()];
    return boundary == '/' || boundary == '\\';
}

std::vector<build::BuildDiagnostic> toProblems(io::IFileSystem& fs, const lang::InspectionReport& report) {
    std::vector<build::BuildDiagnostic> rows;
    rows.reserve(report.findings.size());
    for (const lang::InspectionFinding& finding : report.findings) {
        if (finding.severity == lang::InspectionSeverity::Disabled)
            continue;
        std::string path = finding.file;
        if (!io::isAbsolutePath(path)) {
            const std::string base = report.inspectedFiles.empty()
                                         ? std::string{}
                                         : std::string(io::parentPath(report.inspectedFiles.front()));
            auto joined =
                base.empty()
                    ? Result<std::string>(fail(ErrorCode::InvalidArgument, "clang-tidy returned a relative file path"))
                    : io::joinPath(base, path);
            if (!joined)
                continue;
            path = std::move(*joined);
        }
        auto normalized = io::normalizePath(path);
        if (!normalized)
            continue;
        const auto status = fs.status(*normalized);
        if (!status || status->type != io::FileType::Regular)
            continue;

        build::BuildDiagnostic row;
        row.diagnostic.severity = problemSeverity(finding.severity);
        row.diagnostic.code = finding.ruleId;
        row.diagnostic.message = finding.message;
        row.diagnostic.file = *normalized;
        row.diagnostic.line = static_cast<int>(finding.line);
        row.diagnostic.column = static_cast<int>(finding.column);
        row.path = std::move(*normalized);
        row.jumpable = finding.line > 0;
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

void Workbench::inspectCppCodeAsync(std::string_view scopeDirectory,
                                    std::function<void(Result<lang::InspectionReport>)> done) {
    if (m_shutDown || !m_pool) {
        done(fail(ErrorCode::Busy, "C++ inspection is unavailable while FrostIDE is shutting down"));
        return;
    }
    if (m_projectRoot.empty()) {
        done(fail(ErrorCode::InvalidArgument, "Open a trusted C++ project before running Inspect Code"));
        return;
    }
    const auto admission = m_services.trust.snapshot(m_projectRoot);
    if (!admission.trusted) {
        offerProjectTrust();
        done(fail(ErrorCode::Untrusted, "Inspect Code requires a trusted project"));
        return;
    }
    if (!m_resourceGovernor)
        m_resourceGovernor = std::make_shared<sched::ResourceGovernor>();
    const bool buildActive = (m_buildService && m_buildService->state() == build::BuildState::Running) ||
                             (m_cmake && m_cmake->service().state() == build::BuildState::Running);
    m_resourceGovernor->setLegActive(sched::HeavyLegKind::Build, buildActive);
    auto scope = io::normalizePath(scopeDirectory);
    if (!scope || !io::isAbsolutePath(*scope)) {
        done(fail(ErrorCode::InvalidArgument, "Inspect Code needs an absolute source directory"));
        return;
    }
    if (!isPathWithin(m_projectRoot, *scope)) {
        done(fail(ErrorCode::InvalidArgument, "Inspect Code scope must be inside the open project"));
        return;
    }
    const auto settings = m_services.settings.snapshot();
    lang::ClangdSearch search = lang::defaultClangdSearch(settings->get(settings::key::toolsClangTidyPath));
    std::vector<std::string> plugins = settings->get(settings::key::toolsClangTidyPlugins);

    m_cppInspectionStop.request_stop();
    m_cppInspectionStop = std::stop_source{};
    const std::stop_token stop = m_cppInspectionStop.get_token();
    const std::uint64_t generation = ++m_cppInspectionGeneration;
    m_cppInspectionTrustRevision = admission.revision;
    const std::string root = m_projectRoot;
    const QPointer<Workbench> self(this);
    InspectionDone publish = [self, generation, root, trustRevision = admission.revision,
                              done = std::move(done)](Result<lang::InspectionReport> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, generation, root, trustRevision, result = std::move(result), done = std::move(done)]() mutable {
                if (!self || self->m_shutDown || generation != self->m_cppInspectionGeneration ||
                    root != self->m_projectRoot)
                    return;
                const auto currentTrust = self->m_services.trust.snapshot(root);
                if (!currentTrust.trusted || currentTrust.revision != trustRevision)
                    return;
                if (!result) {
                    const QString reason = qs(result.error().message());
                    const bool missing = result.error().code() == ErrorCode::NotFound;
                    self->notify(missing ? QStringLiteral("C++ inspection unavailable")
                                         : QStringLiteral("Inspect Code failed"),
                                 reason, ui::NotificationKind::Warning);
                    done(std::unexpected(result.error()));
                    return;
                }

                self->m_lastCppInspectionReport = *result;
                const std::vector<build::BuildDiagnostic> rows = toProblems(self->m_services.fs, *result);
                if (self->m_problems)
                    self->m_problems->model()->setProblems(ui::DiagnosticSource::Inspection, rows);
                if (self->m_window)
                    self->m_window->toolWindows().show(ui::tool_windows::kProblems, false);

                const QString summary = result->executionSuccessful
                                            ? QStringLiteral("clang-tidy %1: %2 finding(s) in %3 file(s)")
                                                  .arg(qs(result->toolVersion))
                                                  .arg(static_cast<qulonglong>(rows.size()))
                                                  .arg(static_cast<qulonglong>(result->inspectedFiles.size()))
                                            : QStringLiteral("clang-tidy exited %1; %2 finding(s) came from its output")
                                                  .arg(result->exitCode)
                                                  .arg(static_cast<qulonglong>(rows.size()));
                self->notify(QStringLiteral("Inspect Code"), summary,
                             result->executionSuccessful ? ui::NotificationKind::Info : ui::NotificationKind::Warning);
                done(std::move(result));
            },
            Qt::QueuedConnection);
    };

    std::shared_ptr<lang::ICompileCommandsProvider> commands = m_compileCommands;
    if (!commands) {
        if (build::CMakeService* cmake = cmakeService()) {
            const std::string buildDirectory = cmake->activeBuildDirectory();
            if (!buildDirectory.empty()) {
                commands = std::make_shared<lang::BuildDirectoryCompileCommandsProvider>(
                    m_services.fs, [directory = buildDirectory] { return directory; });
            }
        }
    }

    m_pool
        ->spawn(sched::Lane::Background,
                inspectCppTask(&m_services.fs, &m_services.launcher, &m_services.trust, admission.revision,
                               std::move(commands), m_resourceGovernor, root, std::move(*scope), std::move(search),
                               std::move(plugins), stop, std::move(publish)))
        .detach();
}

Result<void> Workbench::exportCppInspectionSarif(std::string_view outputPath) {
    if (!m_lastCppInspectionReport)
        return fail(ErrorCode::NotFound, "Run Inspect Code before exporting SARIF");
    const auto trust = m_services.trust.snapshot(m_projectRoot);
    if (!trust.trusted || !m_cppInspectionTrustRevision || trust.revision != *m_cppInspectionTrustRevision)
        return fail(ErrorCode::Untrusted, "Project trust changed after the inspection; run Inspect Code again");
    auto normalized = io::normalizePath(outputPath);
    if (!normalized)
        return std::unexpected(normalized.error());
    return lang::writeSarifReport(m_services.fs, *normalized, *m_lastCppInspectionReport);
}

} // namespace frost::app
