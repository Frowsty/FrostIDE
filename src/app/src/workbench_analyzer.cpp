// SPDX-License-Identifier: Apache-2.0
// Native AST/CFG jobs retain trust, exact editor images, database bytes and provider identity.
#include "frost/app/workbench.hpp"
#include "frost/lang/analyzer.hpp"
#include "frost/lang/uri.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "trusted_project_launcher.hpp"
#include "language_bridge.hpp"
#include "cmake_integration.hpp"
#include "frost/build/cmake_service.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <map>
namespace frost::app {
namespace {
using Images=std::map<std::string,std::string>;
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


sched::Task<void> nativeTask(io::IFileSystem* fs,io::IProcessLauncher* launcher,project::TrustStore* trust,
    std::uint64_t revision,lang::AnalyzerRequest request,std::shared_ptr<sched::ResourceGovernor> governor,
    std::function<void(Result<lang::AnalyzerReport>)> done) {
    TrustedProjectLauncher admitted(*launcher,*trust,request.projectRoot,revision,request.stop);
    request.governor=governor.get();
    done(lang::runFrostAnalyzer(admitted,*fs,request));
    co_return;
}
}
void Workbench::scheduleCppAnalyzer(std::string path,bool saved) {
    // Every edit invalidates running native reports before the debounce; file changes may affect headers.
    ++m_cppAnalyzerTicket;
    if(m_cppAnalyzerExportCheck) {
        m_cppAnalyzerExportCheck={};m_lastCppInspectionReport.reset();
        if(m_problems) m_problems->model()->setProblems(ui::DiagnosticSource::Inspection,{});
    }
    if(m_cppAnalyzerRunning) m_cppInspectionStop.request_stop();
    if(m_shutDown || !isCFamilySource(path) || m_projectRoot.empty()) return;
    const auto settings=m_services.settings.snapshot();
    if(!settings->get(settings::key::cppAnalyzerEnabled) || settings->get(settings::key::analysisPowerSave) ||
       (saved && !settings->get(settings::key::cppAnalyzerOnSave))) return;
    if(!m_services.trust.isTrusted(m_projectRoot)) return;
    const auto ticket=m_cppAnalyzerTicket;
    QTimer::singleShot(saved ? 0 : 900,this,[this,ticket,path=std::move(path)] {
        if(ticket!=m_cppAnalyzerTicket || m_shutDown) return;
        const auto settings=m_services.settings.snapshot();
        if(settings->get(settings::key::cppAnalyzerEnabled) && !settings->get(settings::key::analysisPowerSave))
            inspectNativeCppAsync(path,true);
    });
}
void Workbench::inspectNativeCppAsync(std::string_view scopePath,bool automatic) {
    if(m_shutDown || !m_pool || !m_clangd || m_projectRoot.empty()) return;
    const auto permission=m_services.trust.snapshot(m_projectRoot);
    if(!permission.trusted) { if(!automatic) offerProjectTrust(); return; }
    const auto database=m_clangd->compileCommands();
    if(!database) { if(!automatic) notify(QStringLiteral("Native C++ analysis unavailable"),QStringLiteral("No actual compilation database is available."),ui::NotificationKind::Warning); return; }
    auto scope=io::normalizePath(scopePath);
    if(!scope || !io::isAbsolutePath(*scope) || !isPathWithin(m_projectRoot,*scope)) return;
    const auto settings=m_services.settings.snapshot();
    std::vector<std::string> entries;
#define FROST_ANALYZER_RULE(id,title,category,level,applicability,key,cfg) { const auto value=settings->get(settings::Key<std::string>{key}); if(value!="default") entries.emplace_back(std::string(id)+"="+value); }
#include "../../../resources/analyzer_rules.inc"
#undef FROST_ANALYZER_RULE
    const auto& overrides=settings->get(settings::key::cppAnalyzerRules);
    entries.insert(entries.end(),overrides.begin(),overrides.end());
    auto profile=lang::analyzerProfile(entries);
    if(!profile) { if(!automatic) notify(QStringLiteral("Native analyzer profile invalid"),qs(profile.error().message()),ui::NotificationKind::Warning); return; }
    lang::AnalyzerRequest request;
    request.executable=settings->get(settings::key::toolsFrostAnalyzerPath);
    if(request.executable.empty()) request.executable=QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef _WIN32
        QStringLiteral("frost-analyzer.exe")
#else
        QStringLiteral("frost-analyzer")
#endif
    ).toStdString();
    request.projectRoot=m_projectRoot;request.compilationDatabasePath=database->file;
    request.scope.kind=automatic ? lang::InspectionScopeKind::File : lang::InspectionScopeKind::Directory;
    request.scope.paths={*scope};request.profile=std::move(*profile);
    if(automatic) {
        const auto extension=QFileInfo(qs(*scope)).suffix().toLower();
        if(extension=="h" || extension=="hpp" || extension=="hh" || extension=="hxx" || extension=="inl") {
            // Headers have no independent command; analyze the actual project TU configurations.
            request.scope.kind=lang::InspectionScopeKind::Project;request.scope.paths={m_projectRoot};
        }
    }
    Images images;
    for(const auto& document:m_documents->documents()) if(!document->path().empty()) {
        images.emplace(lang::documentKey(document->path()),document->document().text().text());
        request.overlays.emplace_back(document->path(),document->document().text().text());
    }
    if(!m_resourceGovernor) m_resourceGovernor=std::make_shared<sched::ResourceGovernor>();
    const bool buildActive=(m_buildService && m_buildService->state()==build::BuildState::Running) ||
        (cmakeService() && cmakeService()->state()==build::BuildState::Running);
    m_resourceGovernor->setLegActive(sched::HeavyLegKind::Build,buildActive);
    m_cppInspectionStop.request_stop();m_cppInspectionStop=std::stop_source{};
    request.stop=m_cppInspectionStop.get_token();
    const auto generation=++m_cppInspectionGeneration;
    const auto ticket=++m_cppAnalyzerTicket;
    m_cppInspectionTrustRevision=permission.revision;m_cppAnalyzerRunning=true;
    const auto root=m_projectRoot;const auto server=m_clangd;const auto revision=permission.revision;
    QPointer<Workbench> self(this);
    auto publish=[self,generation,ticket,root,server,revision,images=std::move(images),settings,automatic,databasePath=database->file](Result<lang::AnalyzerReport> result) mutable {
        if(!self) return;
        QMetaObject::invokeMethod(self.data(),[self,generation,ticket,root,server,revision,images=std::move(images),settings,automatic,databasePath,result=std::move(result)]() mutable {
            if(!self || self->m_shutDown || self->m_cppInspectionGeneration!=generation || self->m_cppAnalyzerTicket!=ticket) return;
            self->m_cppAnalyzerRunning=false;
            const auto permission=self->m_services.trust.snapshot(root);
            if(root!=self->m_projectRoot || server!=self->m_clangd || !permission.trusted || permission.revision!=revision || settings!=self->m_services.settings.snapshot()) return;
            const auto database=self->m_clangd->compileCommands();
            if(!database || database->file!=databasePath) return;
            Images current;
            for(const auto& document:self->m_documents->documents()) if(!document->path().empty()) current.emplace(lang::documentKey(document->path()),document->document().text().text());
            if(current!=images) return;
            if(!result) { if(!automatic) self->notify(QStringLiteral("Native C++ analysis unavailable"),qs(result.error().message()),ui::NotificationKind::Warning); return; }
            if(!lang::validateAnalyzerInputs(self->m_services.fs,*result)) return;
            auto retained=std::make_shared<lang::AnalyzerReport>(std::move(*result));
            self->m_cppAnalyzerExportCheck=[self,retained,images,settings,root,server,revision,databasePath]() -> Result<void> {
                if(!self || self->m_services.settings.snapshot()!=settings) return fail(ErrorCode::Busy,"Analyzer settings changed");
                const auto permission=self->m_services.trust.snapshot(root);
                if(self->m_projectRoot!=root || self->m_clangd!=server || !permission.trusted || permission.revision!=revision)
                    return fail(ErrorCode::Busy,"Analyzer provider or project trust changed");
                const auto database=self->m_clangd->compileCommands();
                if(!database || database->file!=databasePath) return fail(ErrorCode::Busy,"Analyzer compilation database changed");
                Images current;
                for(const auto& document:self->m_documents->documents()) if(!document->path().empty()) current.emplace(lang::documentKey(document->path()),document->document().text().text());
                if(current!=images) return fail(ErrorCode::Busy,"Analyzer editor snapshots changed");
                return lang::validateAnalyzerInputs(self->m_services.fs,*retained);
            };
            result=*retained;
            self->m_lastCppInspectionReport=result->inspection;
            const auto rows=toProblems(self->m_services.fs,result->inspection);
            if(self->m_problems) self->m_problems->model()->setProblems(ui::DiagnosticSource::Inspection,rows);
            if(!automatic && self->m_window) self->m_window->toolWindows().show(ui::tool_windows::kProblems,false);
            if(!automatic) self->notify(QStringLiteral("Native C++ analysis"),QStringLiteral("%1 finding(s); %2 complete and %3 failed configuration(s). MISRA and Clazy are unsupported.").arg(static_cast<qulonglong>(rows.size())).arg(static_cast<qulonglong>(result->completedConfigurations)).arg(static_cast<qulonglong>(result->failedConfigurations)),result->inspection.executionSuccessful ? ui::NotificationKind::Info : ui::NotificationKind::Warning);
        },Qt::QueuedConnection);
    };
    m_pool->spawn(sched::Lane::Background,nativeTask(&m_services.fs,&m_services.launcher,&m_services.trust,revision,std::move(request),m_resourceGovernor,std::move(publish))).detach();
}
}
