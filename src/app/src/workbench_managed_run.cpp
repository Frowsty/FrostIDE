// SPDX-License-Identifier: Apache-2.0
// Actual evaluated managed target run recipes, with deferred trust/input and post-build target receipts.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <QCryptographicHash>

#include <algorithm>
namespace frost::app {
std::vector<run::RunConfiguration> Workbench::managedRunConfigurations() const {
    const auto model = managedProjectEvaluation();
    if (!model)
        return {};
    auto target = build::managedConsoleTarget(model->request, model->output);
    if (!target)
        return {};
    const auto identity = model->request.project + "\n" + model->request.configuration + "\n" +
                          model->request.targetFramework + "\n" + model->request.runtimeIdentifier;
    run::RunConfiguration config;
    config.id =
        "managed.sdk." +
        QCryptographicHash::hash(QByteArray::fromStdString(identity), QCryptographicHash::Sha256).toHex().toStdString();
    config.name = ".NET: " + model->assemblyName + " (" + model->request.configuration + "; " +
                  model->request.targetFramework + ")";
    config.program = model->request.dotnet;
    config.arguments = {*target};
    config.workingDirectory = std::string(io::parentPath(model->request.project));
    auto build = build::managedProjectBuildSpec(model->request);
    if (!build)
        return {};
    build->arguments.emplace_back("--no-restore");
    config.beforeLaunchTasks.push_back(
        {.program = build->executable, .arguments = build->arguments, .workingDirectory = build->workingDirectory});
    std::vector<run::RunConfiguration> configurations{config};
    for (const auto& profile : model->launchProfiles) {
        if (!profile.unavailable.empty())
            continue;
        auto candidate = config;
        candidate.id +=
            ".profile." + QCryptographicHash::hash(QByteArray::fromStdString(profile.name), QCryptographicHash::Sha256)
                              .toHex()
                              .toStdString();
        candidate.name += " — " + profile.name;
        candidate.arguments.insert(candidate.arguments.end(), profile.arguments.begin(), profile.arguments.end());
        if (!profile.workingDirectory.empty()) {
            auto directory = io::isAbsolutePath(profile.workingDirectory)
                                 ? io::normalizePath(profile.workingDirectory)
                                 : io::joinPath(io::parentPath(model->request.project), profile.workingDirectory);
            if (!directory)
                continue;
            candidate.workingDirectory = *directory;
        }
        auto environment = profile.environment;
        if (!profile.applicationUrl.empty())
            environment["ASPNETCORE_URLS"] = profile.applicationUrl;
        environment["DOTNET_LAUNCH_PROFILE"] = profile.name;
        for (const auto& [name, value] : environment)
            candidate.environment.push_back({name, value});
        configurations.push_back(std::move(candidate));
    }
    return configurations;
}
std::function<Result<void>()> Workbench::managedRunAdmissionFor(const run::RunConfiguration& config) const {
    const auto model = managedProjectEvaluation();
    const auto generated = managedRunConfigurations();
    const bool matches = model && std::ranges::find(generated, config) != generated.end();
    auto* fs = &m_services.fs;
    auto* trust = &m_services.trust;
    const auto root = m_projectRoot;
    const auto revision = m_managedContextTrustRevision;
    const auto lease = m_managedContextLease;
    return [model, matches, fs, trust, root, revision, lease, config]() -> Result<void> {
        if (!matches || !lease->load())
            return fail(ErrorCode::PermissionDenied, "Managed run no longer matches its selected SDK context");
        const auto current = trust->snapshot(root);
        if (!current.trusted || current.revision != revision)
            return fail(ErrorCode::Untrusted, "Managed run trust changed before launch");
        for (const auto& path : {root, model->request.project}) {
            auto physical = fs->physicalPath(path);
            if (!physical || lang::documentKey(*physical) != lang::documentKey(path))
                return fail(ErrorCode::PermissionDenied, "Managed run physical project changed");
        }
        if (!io::isAbsolutePath(config.workingDirectory))
            return fail(ErrorCode::InvalidArgument, "Managed working directory must be absolute");
        auto directory = fs->physicalPath(config.workingDirectory);
        if (!directory || lang::documentKey(*directory) != lang::documentKey(config.workingDirectory) ||
            !trust->isTrusted(*directory))
            return fail(ErrorCode::PermissionDenied, "Managed profile working directory changed or is untrusted");
        if (!model->launchSettingsPath.empty()) {
            auto present = fs->exists(model->launchSettingsPath);
            if (!present || *present != model->launchSettingsPresent)
                return fail(ErrorCode::Busy, "launchSettings presence changed; refresh .NET Project Context");
        }
        if (model->inputFiles.empty())
            return fail(ErrorCode::PermissionDenied, "Managed run has no captured SDK input receipts");
        for (const auto& [path, bytes] : model->inputFiles) {
            auto content = fs->readFileSnapshot(path, 2U * 1024U * 1024U);
            if (!content || *content != bytes)
                return fail(ErrorCode::Busy, "Managed project/import changed; refresh .NET Project Context");
        }
        return {};
    };
}
std::function<Result<run::LaunchPlan>(run::LaunchPlan, std::stop_token)>
Workbench::managedRunPlanFor(const run::RunConfiguration& config) const {
    auto admission = managedRunAdmissionFor(config);
    const auto model = managedProjectEvaluation();
    auto* launcher = &m_services.launcher;
    auto* fs = &m_services.fs;
    auto* trust = &m_services.trust;
    return [admission = std::move(admission), model, launcher, fs, trust,
            config](run::LaunchPlan plan, std::stop_token stop) -> Result<run::LaunchPlan> {
        if (auto valid = admission(); !valid)
            return std::unexpected(valid.error());
        if (plan.program != config.program || plan.arguments != config.arguments || !model)
            return fail(ErrorCode::PermissionDenied, "Managed executable plan lost its captured identity");
        auto spec = build::managedProjectQuerySpec(model->request);
        if (!spec)
            return std::unexpected(spec.error());
        auto receipt = io::runToCompletion(*launcher, *spec, {}, std::chrono::minutes(2), std::move(stop));
        if (!receipt)
            return std::unexpected(receipt.error());
        if (receipt->exitCode != 0)
            return fail(ErrorCode::IoError, "Managed target reevaluation failed after build");
        auto properties = build::parseManagedProjectProperties(receipt->stdoutBytes);
        if (!properties)
            return std::unexpected(properties.error());
        auto target = build::managedConsoleTarget(model->request, *properties);
        if (!target)
            return std::unexpected(target.error());
        if (config.arguments.empty() || *target != config.arguments.front())
            return fail(ErrorCode::Busy, "Managed TargetPath changed during build; refresh .NET Project Context");
        auto physical = fs->physicalPath(*target);
        if (!physical)
            return std::unexpected(physical.error());
        auto status = fs->status(*physical);
        if (!status)
            return std::unexpected(status.error());
        if (status->type != io::FileType::Regular || !trust->isTrusted(io::parentPath(*physical)))
            return fail(ErrorCode::PermissionDenied, "Managed built assembly is outside trusted physical outputs");
        plan.arguments[0] = *physical;
        if (auto valid = admission(); !valid)
            return std::unexpected(valid.error());
        return plan;
    };
}
} // namespace frost::app
