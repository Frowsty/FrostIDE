// SPDX-License-Identifier: Apache-2.0
// Owned editor/player process admission, private endpoint startup and CLI-backed Engine view.
#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "engine_integration_internal.hpp"
#include "engine_native_host_internal.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <format>

namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

std::string secureToken() {
    std::string token;
    token.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; ++i) {
        const auto value = QRandomGenerator::system()->generate64();
        for (unsigned shift = 0; shift < 64; shift += 4)
            token += hex[(value >> shift) & 0xfu];
    }
    return token;
}

bool reservedArgument(std::string_view value) {
    for (const std::string_view key : {"--project", "--config-dir", "--automation-port", "--automation-discovery-file",
                                       "--no-automation", "--token", "--automation-token"})
        if (value == key || (value.starts_with(key) && value.size() > key.size() && value[key.size()] == '='))
            return true;
    return false;
}
} // namespace

std::vector<run::RunConfiguration> EngineIntegration::Impl::generated() const {
    std::vector<run::RunConfiguration> out;
    if (!available)
        return out;
    for (std::size_t index = 0; index < installations.size(); ++index) {
        const auto& installation = installations[index];
        const auto digest = QCryptographicHash::hash(QByteArray::fromStdString(lang::documentKey(installation.program)),
                                                     QCryptographicHash::Sha256)
                                .toHex()
                                .left(16)
                                .toStdString();
        run::RunConfiguration editor;
        editor.id = "engine.editor." + digest;
        editor.name = installation.preset.empty() ? "FrostEditor" : "FrostEditor (" + installation.preset + ")";
        editor.type = run::RunType::FrostEditor;
        editor.program = installation.program;
        editor.engineProjectPath = selectedProject;
        editor.workingDirectory = std::string(io::parentPath(installation.program));
        editor.arguments = workbench.m_options.engineEditorArguments;
        out.push_back(std::move(editor));
        if (!installation.playerProgram.empty()) {
            run::RunConfiguration player;
            player.id = "engine.player." + digest;
            player.name = installation.preset.empty() ? "Frost Player" : "Frost Player (" + installation.preset + ")";
            player.type = run::RunType::FrostPlayer;
            player.program = installation.playerProgram;
            player.engineProjectPath = selectedProject;
            player.workingDirectory = std::string(io::parentPath(installation.playerProgram));
            out.push_back(std::move(player));
        }
    }
    return out;
}

Result<void> EngineIntegration::configureTools(std::string editorPath, std::string configDirectory,
                                               std::string automationDirectory, std::vector<std::string> arguments) {
    auto& impl = *m_impl;
    const auto trust = impl.workbench.m_services.trust.snapshot(impl.root);
    if (!impl.available || !trust.trusted)
        return fail(ErrorCode::Untrusted, "Open and trust a FrostEngine project before configuring its tools");
    for (const auto& path : {editorPath, configDirectory, automationDirectory})
        if (!io::isAbsolutePath(path))
            return fail(ErrorCode::InvalidArgument, "Engine tool and configuration paths must be absolute");
    for (const auto& argument : arguments)
        if (argument.find('\0') != std::string::npos || reservedArgument(argument))
            return fail(ErrorCode::InvalidArgument,
                        "Extra editor arguments cannot replace project, pairing or credential flags");
    impl.invalidate(true);
    impl.workbench.m_options.frostEditorPath = std::move(editorPath);
    impl.workbench.m_options.engineConfigDirectory = std::move(configDirectory);
    impl.workbench.m_options.engineAutomationDirectory = automationDirectory;
    impl.workbench.m_options.engineEditorArguments = std::move(arguments);
    impl.chosenProgram.clear();
    impl.automationDirectory = std::move(automationDirectory);
    impl.trustRevision = trust.revision;
    return impl.refresh();
}

Result<build::RunId> EngineIntegration::startEditor() {
    auto& impl = *m_impl;
    if (!impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "Trust the project before starting FrostEditor");
    if (!impl.workbench.m_runController)
        return fail(ErrorCode::Unsupported, "The Run controller is unavailable");
    const auto generated = impl.generated();
    const auto found = std::ranges::find(generated, run::RunType::FrostEditor, &run::RunConfiguration::type);
    if (found == generated.end())
        return fail(ErrorCode::NotFound, "FrostEditor is unavailable; locate a built editor first");
    impl.workbench.m_runController->refreshStartupCombo();
    return impl.workbench.m_runController->runConfiguration(found->id);
}

Result<run::RunConfiguration> EngineIntegration::prepareRun(const run::RunConfiguration& configuration) {
    return m_impl->prepare(configuration);
}

Result<run::RunConfiguration> EngineIntegration::Impl::prepare(const run::RunConfiguration& configuration) {
    if (!run::isFrostEngineRunType(configuration.type))
        return fail(ErrorCode::InvalidArgument, "This configuration is not a FrostEngine run");
    if (!admitted(epoch, root, trustRevision))
        return fail(ErrorCode::Untrusted, "Project trust changed before starting FrostEngine");
    if (ownedRun || (runAdmission && runAdmission->active.load()))
        return fail(ErrorCode::Busy, "A FrostEngine run is already starting or running");
    for (const auto& argument : configuration.arguments)
        if (reservedArgument(argument))
            return fail(ErrorCode::InvalidArgument,
                        "Engine session flags are supplied by FrostIDE, not extra arguments");
    const bool player = configuration.type == run::RunType::FrostPlayer;
    QString config = qs(workbench.m_options.engineConfigDirectory);
    if (config.isEmpty()) {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!QDir::isAbsolutePath(base))
            return fail(ErrorCode::NotFound, "No per-user directory is available for the engine session");
        const auto profileKey =
            QCryptographicHash::hash(QByteArray::fromStdString(projectKey + "|" + configuration.program + "|" +
                                                               std::string(run::toString(configuration.type))),
                                     QCryptographicHash::Sha256)
                .toHex()
                .left(32);
        // Reuse a private per-project/build profile so owned editor preferences persist and
        // repeated Run does not create an unbounded series of configuration directories.
        config = QDir(base).filePath(QStringLiteral("engine-sessions/") + QString::fromLatin1(profileKey));
    }
    if (!QDir::isAbsolutePath(config))
        return fail(ErrorCode::InvalidArgument, "The engine configuration directory must be absolute");
    clearSession(false);
    ownedConfigDirectory = config.toUtf8().toStdString();
    ownedOutput.clear();
    ownedSecret = player ? secureToken() : std::string{};
    auto prepared = configuration;
    // launchPlanFor supplies the declared --project argument exactly once.
    prepared.arguments.insert(prepared.arguments.end(), {"--automation-port", "0"});
    if (player) {
        const std::string discovery =
            QDir(config).filePath(QStringLiteral("player-instance.json")).toUtf8().toStdString();
        prepared.arguments.insert(prepared.arguments.end(), {"--automation-discovery-file", discovery});
    } else {
        prepared.arguments.insert(prepared.arguments.end(), {"--config-dir", ownedConfigDirectory});
    }
    // Reject inherited fixed engine secrets: editors generate their own and player launches get a
    // fresh in-memory secret. The transient configuration and private streams are never persisted.
    prepared.environment.erase(
        std::remove_if(
            prepared.environment.begin(), prepared.environment.end(),
            [](const auto& entry) {
                return qs(entry.name).compare(QStringLiteral("FROST_AUTOMATION_TOKEN"), Qt::CaseInsensitive) == 0;
            }),
        prepared.environment.end());
    prepared.environment.push_back(
        {"FROST_AUTOMATION_TOKEN", player ? std::optional<std::string>(ownedSecret) : std::nullopt});
    runAdmission = std::make_shared<EngineRunAdmission>();
    runAdmission->id = configuration.id;
    runAdmission->program = configuration.program;
    runAdmission->project = configuration.engineProjectPath;
    runAdmission->root = root;
    runAdmission->trustRevision = trustRevision;
    runAdmission->trust = &workbench.m_services.trust;
    runAdmission->fs = &workbench.m_services.fs;
    runAdmission->projectKey = projectKey;
    runAdmission->physicalAdmission = physicalAdmission;
    runAdmission->configDirectory = ownedConfigDirectory;
    setStatus(QStringLiteral("Starting FrostEngine..."));
    updateControls();
    return prepared;
}

std::function<Result<void>()> EngineIntegration::launchAdmissionFor(const run::RunConfiguration& configuration) const {
    const auto admission = m_impl->runAdmission;
    if (!admission || admission->id != configuration.id || admission->program != configuration.program ||
        admission->project != configuration.engineProjectPath)
        return
            []() -> Result<void> { return fail(ErrorCode::PermissionDenied, "No matching engine launch reservation"); };
    return [admission]() -> Result<void> {
        const auto trust = admission->trust->snapshot(admission->root);
        if (!admission->active.load())
            return fail(ErrorCode::Cancelled, "The engine launch reservation was cancelled");
        if (!retainEngineProjectIdentity(*admission->fs, admission->root, admission->projectKey,
                                         admission->physicalAdmission))
            return fail(ErrorCode::PermissionDenied, "The physical project changed before engine launch");
        auto project = admission->fs->physicalPath(admission->project);
        if (!project)
            return std::unexpected(project.error());
        auto status = admission->fs->status(*project);
        if (!status)
            return std::unexpected(status.error());
        if (status->type != io::FileType::Directory &&
            (status->type != io::FileType::Regular || !lang::isFrostEngineProjectFileName(io::fileName(*project))))
            return fail(ErrorCode::InvalidArgument, "The engine project must be a directory or project file");
        const std::string projectRoot =
            status->type == io::FileType::Regular ? std::string(io::parentPath(*project)) : *project;
        if (lang::documentKey(projectRoot) != admission->projectKey)
            return fail(ErrorCode::PermissionDenied, "The run configuration belongs to a different physical project");
        {
            std::scoped_lock lock(admission->resolutionMutex);
            if (admission->resolvedProject &&
                lang::documentKey(*admission->resolvedProject) != lang::documentKey(*project))
                return fail(ErrorCode::PermissionDenied,
                            "The engine project target changed after its launch was admitted");
            if (!admission->resolvedProject)
                admission->resolvedProject = *project;
        }
        if (auto created = admission->fs->createDirectories(admission->configDirectory); !created)
            return std::unexpected(created.error());
#ifndef _WIN32
        if (!QFile::setPermissions(qs(admission->configDirectory),
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return fail(ErrorCode::PermissionDenied, "Cannot protect the engine session directory");
#endif
        const auto currentTrust = admission->trust->snapshot(admission->root);
        if (!trust.trusted || trust.revision != admission->trustRevision || !currentTrust.trusted ||
            currentTrust.revision != admission->trustRevision || !admission->active.load() ||
            !admission->physicalAdmission->load())
            return fail(ErrorCode::Untrusted, "Project trust changed before the engine process could start");
        return {};
    };
}

std::function<Result<run::LaunchPlan>(run::LaunchPlan)>
EngineIntegration::launchPlanFor(const run::RunConfiguration& configuration) const {
    const auto admission = m_impl->runAdmission;
    if (!admission || admission->id != configuration.id || admission->program != configuration.program ||
        admission->project != configuration.engineProjectPath)
        return {};
    return [admission](run::LaunchPlan plan) -> Result<run::LaunchPlan> {
        if (!admission->active.load() ||
            !retainEngineProjectIdentity(*admission->fs, admission->root, admission->projectKey,
                                         admission->physicalAdmission))
            return fail(ErrorCode::PermissionDenied, "The engine launch no longer belongs to the selected project");
        if (plan.arguments.size() < 2 || plan.arguments.front() != "--project" ||
            plan.arguments[1] != admission->project)
            return fail(ErrorCode::InvalidArgument, "Engine launch plan lost its declared project argument");
        auto project = admission->fs->physicalPath(admission->project);
        if (!project)
            return std::unexpected(project.error());
        auto status = admission->fs->status(*project);
        if (!status)
            return std::unexpected(status.error());
        const std::string directory =
            status->type == io::FileType::Regular ? std::string(io::parentPath(*project)) : *project;
        if (lang::documentKey(directory) != admission->projectKey)
            return fail(ErrorCode::PermissionDenied,
                        "Engine launch project resolves outside its original physical root");
        // The stored configuration remains unchanged. The child receives an immutable physical
        // target instead of a lexical alias that could be retargeted after worker admission.
        {
            std::scoped_lock lock(admission->resolutionMutex);
            if (!admission->resolvedProject ||
                lang::documentKey(*admission->resolvedProject) != lang::documentKey(*project))
                return fail(ErrorCode::PermissionDenied,
                            "The resolved engine project changed during launch preparation");
            plan.arguments[1] = *admission->resolvedProject;
        }
        return plan;
    };
}

void EngineIntegration::runAccepted(run::RunId id, const run::RunConfiguration& configuration) {
    auto& impl = *m_impl;
    if (!impl.runAdmission || !impl.runAdmission->active.load() || impl.runAdmission->id != configuration.id ||
        !impl.admitted(impl.epoch, impl.root, impl.runAdmission->trustRevision)) {
        if (impl.workbench.m_runManager)
            impl.workbench.m_runManager->stop(id);
        return;
    }
    impl.ownedRun = id;
    impl.ownedProcessStarted = false;
    impl.updateControls();
}

void EngineIntegration::processStarted(const run::RunStatus& run) {
    auto& impl = *m_impl;
    if (!impl.ownedRun || *impl.ownedRun != run.id || !impl.runAdmission || !impl.runAdmission->active.load() ||
        impl.runAdmission->id != run.configuration.id ||
        !impl.admitted(impl.epoch, impl.root, impl.runAdmission->trustRevision)) {
        if (impl.workbench.m_runManager)
            impl.workbench.m_runManager->stop(run.id);
        return;
    }
    if (impl.ownedProcessStarted)
        return;
    impl.ownedProcessStarted = true;
    impl.ownedRun = run.id;
    impl.ownedPid = run.processId;
    impl.awaitingOwnedEndpoint = true;
    impl.awaitingOwnedAuthentication = true;
    impl.startupDeadline = impl.workbench.m_services.clock.steadyNow() + std::chrono::seconds(30);
    impl.startupWatch->start();
    impl.updateControls();
}

void EngineIntegration::processOutput(run::RunId runId, run::OutputStream stream, std::string_view bytes) {
    auto& impl = *m_impl;
    if (!impl.ownedRun || *impl.ownedRun != runId || !impl.awaitingOwnedEndpoint || stream != run::OutputStream::Stdout)
        return;
    if (impl.ownedOutput.size() + bytes.size() > 1024U * 1024U) {
        impl.setStatus(QStringLiteral("FrostEngine startup exceeded the private output limit"));
        impl.workbench.m_runManager->stop(runId);
        impl.ownedOutput.clear();
        impl.awaitingOwnedEndpoint = false;
        return;
    }
    impl.ownedOutput.append(bytes);
    impl.endpointFromOutput();
}

void EngineIntegration::Impl::endpointFromOutput() {
    for (;;) {
        const auto end = ownedOutput.find('\n');
        if (end == std::string::npos)
            break;
        const std::string line = ownedOutput.substr(0, end);
        ownedOutput.erase(0, end + 1);
        auto wrapper = lang::parseJson(line);
        if (!wrapper || !wrapper->isObject() || wrapper->boolAt("ok") != true ||
            (wrapper->stringAt("command") != "editor" && wrapper->stringAt("command") != "player"))
            continue;
        const auto* result = wrapper->find("result");
        if (!result || !result->isObject())
            continue;
        auto record = *result;
        const auto port = record.integerAt("port");
        if (record.stringAt("host") != "127.0.0.1" || !port || *port < 1 || *port > 65535 ||
            !io::isAbsolutePath(record.stringAt("projectPath"))) {
            setStatus(QStringLiteral("FrostEngine startup reported an invalid endpoint"));
            stopOwnedHost();
            awaitingOwnedEndpoint = false;
            return;
        }
        if (!ownedSecret.empty())
            record.set("token", ownedSecret);
        awaitingOwnedEndpoint = false;
        ownedOutput.clear();
        if (nativeHost) {
            // The canonical --project target was fixed by worker launch admission. Reject any
            // different wire root here; EngineSession revalidates physical identity on its worker.
            if (lang::documentKey(record.stringAt("projectPath")) != projectKey) {
                setStatus(QStringLiteral("Native engine startup reported a different project"));
                stopOwnedHost();
                return;
            }
            nativeHost->endpoint = std::make_shared<EngineInstanceRecord>(
                EngineInstanceRecord{std::string(record.stringAt("instanceId")), physicalRoot,
                                     static_cast<std::uint16_t>(*port), std::move(record)});
            auto connected = connectInstance(*nativeHost->endpoint);
            if (!connected) {
                setStatus(qs(connected.error().message()));
                stopOwnedHost();
            }
            return;
        }
        const auto currentEpoch = epoch;
        const auto expectedRoot = root;
        const auto originalKey = projectKey;
        const auto admission = physicalAdmission;
        const auto revision = trustRevision;
        const auto runId = ownedRun;
        const auto nativeLease = nativeHost;
        auto* fs = &workbench.m_services.fs;
        QPointer<EngineIntegration> self(&owner);
        background([self, fs, currentEpoch, expectedRoot, originalKey, admission, revision, runId, nativeLease,
                    port = static_cast<std::uint16_t>(*port), record = std::move(record)]() mutable {
            if (!retainEngineProjectIdentity(*fs, expectedRoot, originalKey, admission))
                return;
            auto physical = fs->physicalPath(record.stringAt("projectPath"));
            Result<EngineInstanceRecord> instance =
                fail(ErrorCode::PermissionDenied, "FrostEngine startup reported a different physical project");
            if (physical && lang::documentKey(*physical) == originalKey)
                instance = EngineInstanceRecord{std::string(record.stringAt("instanceId")), *physical, port,
                                                std::move(record)};
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, currentEpoch, expectedRoot, revision, runId, nativeLease,
                 instance = std::move(instance)]() mutable {
                    if (!self)
                        return;
                    auto& impl = *self->m_impl;
                    if (!impl.admitted(currentEpoch, expectedRoot, revision) || impl.ownedRun != runId ||
                        impl.nativeHost != nativeLease || (nativeLease && !nativeLease->active.load()))
                        return;
                    auto connected =
                        instance ? impl.connectInstance(*instance) : Result<void>(std::unexpected(instance.error()));
                    if (!connected) {
                        impl.setStatus(qs(connected.error().message()));
                        impl.stopOwnedHost();
                    }
                },
                Qt::QueuedConnection);
        });
        return;
    }
}

void EngineIntegration::Impl::checkOwnedStartup() {
    if (!awaitingOwnedEndpoint && !awaitingOwnedAuthentication) {
        startupWatch->stop();
        return;
    }
    if (!admitted(epoch, root, trustRevision) || workbench.m_services.clock.steadyNow() >= startupDeadline) {
        setStatus(QStringLiteral("FrostEngine did not advertise an authenticated session in time"));
        stopOwnedHost();
        awaitingOwnedEndpoint = false;
        awaitingOwnedAuthentication = false;
        ownedOutput.clear();
        startupWatch->stop();
    }
}

void EngineIntegration::processFinished(const run::RunResult& result) {
    auto& impl = *m_impl;
    if ((!impl.ownedRun || *impl.ownedRun != result.id) &&
        !(result.id == 0 && impl.runAdmission && impl.runAdmission->id == result.configurationId))
        return;
    const auto crashLease = impl.crashSessions->snapshot();
    const bool preserveCrash = preserveEngineCrashObservation(crashLease, static_cast<std::int64_t>(impl.ownedPid),
                                                              result.stopped, result.exitCode);
    const bool observedExit = preserveCrash && crashLease->live() && !result.stopped && result.exitCode.has_value();
    if (observedExit) {
        (void)impl.crashSessions->lost(crashLease->identity(), true);
        impl.observedCrashLoss(crashLease->identity());
    }
    if (impl.runAdmission)
        impl.runAdmission->active.store(false);
    impl.runAdmission.reset();
    impl.ownedRun.reset();
    impl.ownedPid = 0;
    impl.ownedProcessStarted = false;
    impl.ownedSecret.clear();
    impl.ownedOutput.clear();
    impl.awaitingOwnedEndpoint = false;
    impl.awaitingOwnedAuthentication = false;
    impl.startupWatch->stop();
    impl.clearSession(false, preserveCrash);
    impl.setStatus(result.error ? qs(result.error->message()) : QStringLiteral("Owned engine process exited"));
    impl.updateControls();
}

void EngineIntegration::Impl::refreshProjectContent() {
    const auto generation = ++contentGeneration;
    if (!admitted(epoch, root, trustRevision)) {
        projectView->setUnavailable(QStringLiteral("Trust the project to read its engine content"));
        return;
    }
    if (installations.empty()) {
        projectView->setUnavailable(QStringLiteral("FrostEditor CLI is unavailable; locate a built editor"));
        return;
    }
    projectBusy = true;
    projectView->setUnavailable(QStringLiteral("Reading project settings and assets from FrostEditor..."));
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto revision = trustRevision;
    const auto stop = workStop.get_token();
    const auto program = installations.front().cliProgram;
    const auto expectedPhysicalRoot = selectedProject;
    const auto expectedProjectKey = projectKey;
    const auto admission = physicalAdmission;
    auto* fs = &workbench.m_services.fs;
    auto* launcher = &workbench.m_services.launcher;
    auto* trust = &workbench.m_services.trust;
    QPointer<EngineIntegration> self(&owner);
    background([self, fs, launcher, trust, currentEpoch, expectedRoot, expectedPhysicalRoot, expectedProjectKey,
                admission, revision, stop, program, generation] {
        const auto trusted = [trust, fs, expectedRoot, expectedProjectKey, admission, revision, stop] {
            const auto current = trust->snapshot(expectedRoot);
            return !stop.stop_requested() && current.trusted && current.revision == revision &&
                   retainEngineProjectIdentity(*fs, expectedRoot, expectedProjectKey, admission);
        };
        engine::ProjectContentRequest request;
        request.program = program;
        request.project = expectedPhysicalRoot;
        request.stopToken = stop;
        auto content = engine::loadProjectContent(*fs, *launcher, request, trusted);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, currentEpoch, expectedRoot, revision, generation, content = std::move(content)]() mutable {
                if (!self)
                    return;
                auto& impl = *self->m_impl;
                if (!impl.admitted(currentEpoch, expectedRoot, revision) || impl.contentGeneration != generation)
                    return;
                impl.projectBusy = false;
                if (content) {
                    impl.projectContent = *content;
                    impl.projectView->setContent(std::move(*content));
                } else {
                    impl.projectContent.reset();
                    impl.projectView->setUnavailable(qs(content.error().message()));
                }
            },
            Qt::QueuedConnection);
    });
}

void EngineIntegration::Impl::openAsset(std::string relative) {
    if (!projectContent || !admitted(epoch, root, trustRevision))
        return;
    const auto content = *projectContent;
    const auto admission = physicalAdmission;
    const auto expectedProjectKey = projectKey;
    const auto currentEpoch = epoch;
    const auto expectedRoot = root;
    const auto revision = trustRevision;
    const auto generation = contentGeneration;
    auto* fs = &workbench.m_services.fs;
    QPointer<EngineIntegration> self(&owner);
    background([self, fs, content, admission, expectedProjectKey, currentEpoch, expectedRoot, revision, generation,
                relative = std::move(relative)] {
        if (!retainEngineProjectIdentity(*fs, expectedRoot, expectedProjectKey, admission))
            return;
        auto resolved = engine::resolveProjectAsset(*fs, content, relative);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, currentEpoch, expectedRoot, revision, generation, resolved = std::move(resolved)] {
                if (!self || !self->m_impl->admitted(currentEpoch, expectedRoot, revision) ||
                    self->m_impl->contentGeneration != generation)
                    return;
                if (resolved) {
                    if (self->m_impl->workbench.m_nav)
                        (void)self->m_impl->workbench.m_nav->openFile(*resolved);
                } else
                    self->m_impl->setStatus(qs(resolved.error().message()));
            },
            Qt::QueuedConnection);
    });
}

} // namespace frost::app
