// SPDX-License-Identifier: Apache-2.0
// Generated files and existing-project membership are prepared before one REF-1 Create decision.
#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/generated_sources.hpp"
#include "frost/settings/keys.hpp"

#include "cmake_integration.hpp"
#include "trusted_project_launcher.hpp"
#include "workbench_source_rename.hpp"
#include "workbench_source_rename_preview.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <map>

namespace frost::app {
Result<std::vector<std::string>> Workbench::createSourceTemplate(const SourceTemplateSpec& spec) {
    if (!m_window || !m_documents || !m_nav)
        return fail(ErrorCode::InvalidArgument, "Open a workspace before using source templates");
    QPointer<Workbench> owner(this);
    if (m_solution)
        if (auto allowed = m_solution->requireProjectMutation(); !allowed)
            return std::unexpected(allowed.error());
    auto rendered = lang::renderFileTemplate(spec.file);
    if (!rendered)
        return std::unexpected(rendered.error());
    if (!io::isAbsolutePath(spec.directory) || (!spec.cmakeTargetId.empty() && !spec.vcxprojPath.empty()) ||
        (spec.newCMakeTarget && (!spec.vcxprojPath.empty() || !spec.cmakeTargetId.empty())))
        return fail(ErrorCode::InvalidArgument, "Select one template destination and project membership");
    const auto root = m_projectRoot;
    const auto trust = m_services.trust.snapshot(root);
    if (!trust.trusted)
        return fail(ErrorCode::Untrusted, "Trust the project before creating source templates");
    const auto solution = m_solution;
    const auto evaluation = m_evaluatedGeneration;
    const auto active = m_build ? m_build->activeConfigurationIndex() : std::nullopt;
    const auto cmake = m_cmake ? m_cmake->sharedService() : nullptr;
    const auto model = m_cmake ? m_cmake->shownModel() : nullptr;
    const auto profile = cmake ? cmake->profileGeneration() : 0;
    const auto msbuildSetting = m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath);
    if ((model && !spec.newCMakeTarget && spec.cmakeTargetId.empty()) || (solution && spec.vcxprojPath.empty()))
        return fail(ErrorCode::InvalidArgument, "Choose the actual target/project that owns the generated files");
    std::string configuration, platform;
    if (!spec.vcxprojPath.empty()) {
        if (!solution)
            return fail(ErrorCode::Busy, "Open the owning solution before generating its files");
        const auto id = solution->findProjectByPath(spec.vcxprojPath);
        const auto* info = id ? &solution->project(*id) : nullptr;
        if (!info || !active || *active >= info->configurationMap.size() || !info->configurationMap[*active])
            return fail(ErrorCode::Busy, "Select an active C++ project configuration");
        configuration = info->configurationMap[*active]->configuration;
        platform = info->configurationMap[*active]->platform;
        if (auto blocked = projectEditBlocked(spec.vcxprojPath))
            return fail(ErrorCode::PermissionDenied, blocked->toStdString());
    }
    struct Open {
        std::string path;
        text::Snapshot text;
        const document::FileDocument* instance;
    };
    std::vector<Open> open;
    for (const auto& file : m_documents->documents())
        if (!file->untitled())
            open.push_back({file->path(), file->document().snapshot().text, file.get()});
    auto validate = [owner, root, trust, solution, evaluation, active, cmake, model, profile, open,
                     msbuildSetting]() -> Result<void> {
        if (!owner || owner->m_shutDown)
            return fail(ErrorCode::Cancelled, "Workbench closed during template preparation");
        if (owner->m_projectRoot != root || owner->m_solution != solution ||
            owner->m_evaluatedGeneration != evaluation ||
            (owner->m_build ? owner->m_build->activeConfigurationIndex() : std::nullopt) != active)
            return fail(ErrorCode::Busy, "Project/configuration changed; prepare the template again");
        if (owner->m_solution)
            if (auto allowed = owner->m_solution->requireProjectMutation(); !allowed)
                return allowed;
        const auto now = owner->m_services.trust.snapshot(root);
        if (!now.trusted || now.revision != trust.revision)
            return fail(ErrorCode::Untrusted, "Project trust changed during template preparation");
        if (owner->m_options.msbuildPath.empty() &&
            owner->m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath) != msbuildSetting)
            return fail(ErrorCode::Busy, "MSBuild selection changed; prepare the template again");
        if (owner->m_buildService->state() != build::BuildState::Idle || owner->m_buildService->projectEditInProgress())
            return fail(ErrorCode::Busy, "Wait for the build/project edit");
        std::size_t count = 0;
        for (const auto& file : owner->m_documents->documents())
            if (!file->untitled()) {
                if (count >= open.size() || open[count].instance != file.get() || open[count].path != file->path())
                    return fail(ErrorCode::Busy, "Open document identities changed during template preparation");
                ++count;
            }
        if (count != open.size())
            return fail(ErrorCode::Busy, "Open documents changed during template preparation");
        if (model &&
            (!owner->m_cmake || owner->m_cmake->sharedService() != cmake || !owner->m_cmake->targetsCurrent() ||
             cmake->model() != model || cmake->profileGeneration() != profile || cmake->modelLoading() ||
             cmake->modelStale() || cmake->state() != build::BuildState::Idle))
            return fail(ErrorCode::Busy, "CMake inputs/profile changed; configure and prepare the template again");
        return {};
    };
    if (auto current = validate(); !current)
        return std::unexpected(current.error());
    std::vector<document::FileCreation> creates;
    std::vector<std::string> paths;
    for (auto& file : *rendered) {
        auto path = io::joinPath(spec.directory, file.name);
        if (!path)
            return std::unexpected(path.error());
        paths.push_back(*path);
        creates.push_back({*path, std::move(file.contents)});
    }
    auto host = m_options.projectEditHostPath;
    if (host.empty())
        host = QDir(QCoreApplication::applicationDirPath())
                   .filePath("project-host/ProjectHostVsProbe.exe")
                   .toUtf8()
                   .toStdString();
    auto msbuild = m_options.msbuildPath.empty() ? m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath)
                                                 : m_options.msbuildPath;
    auto* fs = &m_services.fs;
    auto* launcher = &m_services.launcher;
    auto* trustStore = &m_services.trust;
    auto prepared = prepareSourceRenameInBackground(
        m_window,
        [fs, launcher, trustStore, root, trust, open, model, spec, paths, creates = std::move(creates), host, msbuild,
         configuration, platform](std::stop_token stop) mutable -> Result<document::RefactoringPlan> {
            document::RefactoringPlan plan;
            plan.label = "Create " + spec.file.name + " from template";
            plan.creates = std::move(creates);
            auto directory = fs->status(spec.directory);
            if (!directory)
                return std::unexpected(directory.error());
            if (directory->type != io::FileType::Directory)
                return fail(ErrorCode::InvalidArgument, "Choose an existing template destination directory");
            plan.dependencies.push_back({spec.directory, std::nullopt, true});
            for (const auto& creation : plan.creates) {
                auto status = fs->status(creation.path);
                if (status)
                    return fail(ErrorCode::AlreadyExists, "Template destination already exists: " + creation.path);
                if (status.error().code() != ErrorCode::NotFound)
                    return std::unexpected(status.error());
                plan.dependencies.push_back({creation.path, std::nullopt});
            }
            std::map<std::string, RefactoringInputSnapshot> inputs;
            std::size_t capturedBytes = 0;
            auto capture = [&](const std::string& path) -> Result<RefactoringInputSnapshot> {
                if (stop.stop_requested())
                    return fail(ErrorCode::Cancelled, "Template preparation cancelled");
                const auto key = lang::documentKey(path);
                if (const auto found = inputs.find(key); found != inputs.end())
                    return found->second;
                if (inputs.size() >= 1024)
                    return fail(ErrorCode::OutOfRange, "Template input count limit exceeded");
                auto physical = fs->physicalPath(path);
                if (!physical && physical.error().code() != ErrorCode::NotFound)
                    return std::unexpected(physical.error());
                const Open* selected = nullptr;
                for (const auto& file : open) {
                    if (stop.stop_requested())
                        return fail(ErrorCode::Cancelled, "Template preparation cancelled");
                    bool matches = lang::documentKey(file.path) == key;
                    if (physical) {
                        auto identity = fs->physicalPath(file.path);
                        if (!identity)
                            return fail(ErrorCode::Busy, "Cannot establish open input identity");
                        matches |= lang::documentKey(*identity) == lang::documentKey(*physical);
                    }
                    if (matches) {
                        if (selected)
                            return fail(ErrorCode::Busy, "Multiple open aliases refer to a project input");
                        selected = &file;
                    }
                }
                auto input =
                    captureRefactoringInput(*fs, path, selected ? std::optional(selected->text.text()) : std::nullopt);
                if (input) {
                    const auto bytes =
                        (input->disk ? input->disk->size() : 0) + (input->buffer ? input->buffer->size() : 0);
                    if (bytes > 64 * 1024 * 1024 - capturedBytes)
                        return fail(ErrorCode::OutOfRange, "Combined template input limit exceeded");
                    capturedBytes += bytes;
                    inputs.emplace(key, *input);
                }
                return input;
            };
            if (model) {
                std::vector<project::CMakeInputSnapshot> snapshots;
                for (const auto& file : model->inputs)
                    if (!file.cmake) {
                        if (stop.stop_requested())
                            return fail(ErrorCode::Cancelled, "Template preparation cancelled");
                        auto input = capture(file.path);
                        if (!input)
                            return std::unexpected(input.error());
                        if (!input->disk)
                            return fail(ErrorCode::NotFound, "CMake input is missing");
                        if (auto same = refactoringInputMatchesEvaluation(*input, input->disk); !same)
                            return std::unexpected(same.error());
                        if (auto read = appendRefactoringDependency(plan, *input); !read)
                            return std::unexpected(read.error());
                        snapshots.push_back({file.path, *input->disk});
                    }
                for (const auto& path : {model->replyIndex, model->buildDirectory + "/CMakeCache.txt"}) {
                    auto input = capture(path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (!input->disk)
                        return fail(ErrorCode::NotFound, "CMake reply/cache identity is missing");
                    if (auto clean = refactoringInputMatchesEvaluation(*input, input->disk); !clean)
                        return std::unexpected(clean.error());
                    if (io::fileName(path) == "CMakeCache.txt")
                        if (auto current = project::validateTemplateCMakeCache(*model, *input->disk); !current)
                            return std::unexpected(current.error());
                    if (auto read = appendRefactoringDependency(plan, *input); !read)
                        return std::unexpected(read.error());
                }
                std::vector<project::CMakeMembershipPlan> edits;
                if (spec.newCMakeTarget) {
                    auto changed =
                        project::planCMakeTemplateTarget(*model, spec.file.name, spec.staticLibrary, paths, snapshots);
                    if (!changed)
                        return std::unexpected(changed.error());
                    edits.push_back(std::move(*changed));
                } else {
                    const auto selected =
                        std::ranges::find(model->targets, spec.cmakeTargetId, &project::CMakeTarget::id);
                    if (selected == model->targets.end())
                        return fail(ErrorCode::Busy, "Selected CMake target is no longer configured");
                    auto changed = project::planCMakeGeneratedSources(
                        *model, static_cast<std::size_t>(selected - model->targets.begin()), paths, snapshots);
                    if (!changed)
                        return std::unexpected(changed.error());
                    edits = std::move(*changed);
                }
                for (const auto& edit : edits) {
                    auto input = capture(edit.definitionPath);
                    if (!input)
                        return std::unexpected(input.error());
                    if (auto added = appendRefactoringReplacement(plan, *input, edit.original, edit.replacement);
                        !added)
                        return std::unexpected(added.error());
                }
            } else if (!spec.vcxprojPath.empty()) {
                project::ProjectEdit edit;
                edit.kind = project::ProjectEditKind::AddFiles;
                edit.root = root;
                edit.definition = spec.vcxprojPath;
                edit.files = paths;
                edit.configuration = configuration;
                edit.platform = platform;
                edit.transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                edit.commit = false;
                edit.beforeFileCreate = true;
                TrustedProjectLauncher admitted(*launcher, *trustStore, root, trust.revision, stop);
                if (msbuild.empty()) {
                    auto found = build::findMsBuild(admitted, *fs);
                    if (!found)
                        return std::unexpected(found.error());
                    msbuild = found->msbuildPath;
                }
                auto definitionInput = capture(edit.definition);
                auto filters = capture(edit.definition + ".filters");
                if (!definitionInput)
                    return std::unexpected(definitionInput.error());
                if (!filters)
                    return std::unexpected(filters.error());
                if (auto clean = refactoringInputMatchesEvaluation(*definitionInput, definitionInput->disk); !clean)
                    return std::unexpected(clean.error());
                if (auto clean = refactoringInputMatchesEvaluation(*filters, filters->disk); !clean)
                    return std::unexpected(clean.error());
                auto answer =
                    project::runProjectEditPlan(admitted, {host, msbuild, std::chrono::seconds(120)}, edit, stop);
                if (!answer)
                    return std::unexpected(answer.error());
                if (auto same =
                        refactoringInputMatchesEvaluation(*definitionInput, std::optional(answer->projectBefore));
                    !same)
                    return std::unexpected(same.error());
                if (auto same = refactoringInputMatchesEvaluation(*filters, answer->filtersBefore); !same)
                    return std::unexpected(same.error());
                if (auto added = appendRefactoringReplacement(plan, *definitionInput, answer->projectBefore,
                                                              answer->projectAfter);
                    !added)
                    return std::unexpected(added.error());
                if (auto read = appendRefactoringDependency(plan, *filters); !read)
                    return std::unexpected(read.error());
                if (answer->filtersBefore && answer->filtersAfter && *answer->filtersBefore != *answer->filtersAfter)
                    if (auto added =
                            appendRefactoringReplacement(plan, *filters, *answer->filtersBefore, *answer->filtersAfter);
                        !added)
                        return std::unexpected(added.error());
                for (const auto& dependency : answer->imports) {
                    auto input = capture(dependency.path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (auto same = refactoringInputMatchesEvaluation(*input, std::optional(dependency.bytes)); !same)
                        return std::unexpected(same.error());
                    if (auto read = appendRefactoringDependency(plan, *input); !read)
                        return std::unexpected(read.error());
                }
            } else {
                auto lists = capture(root + "/CMakeLists.txt");
                if (!lists)
                    return std::unexpected(lists.error());
                if (lists->disk || spec.newCMakeTarget || !spec.cmakeTargetId.empty())
                    return fail(ErrorCode::Busy, "Configure CMake before generating project members");
                if (auto read = appendRefactoringDependency(plan, *lists); !read)
                    return std::unexpected(read.error());
            }
            return plan;
        },
        "Prepare source template");
    if (!owner)
        return fail(ErrorCode::Cancelled, "Workbench closed during template preparation");
    if (!prepared)
        return std::unexpected(prepared.error());
    if (auto current = validate(); !current)
        return std::unexpected(current.error());
    // Every proposed definition and file is shown before the one durable decision.
    if (!confirmSourceTemplatePlan(*prepared))
        return fail(ErrorCode::Cancelled, "Template preview declined; no files were created");
    if (!owner)
        return fail(ErrorCode::Cancelled, "Workbench closed during template preview");
    if (auto current = validate(); !current)
        return std::unexpected(current.error());
    prepared->revalidateContext = std::move(validate);
    auto outcome = applyRefactoring(*prepared);
    if (!outcome)
        return std::unexpected(outcome.error());
    if (outcome->status == document::RefactoringStatus::PendingRecovery)
        return fail(ErrorCode::Busy, "Template operation committed and awaits recovery; do not retry creation");
    if (!owner)
        return paths; // Settled creation remains successful if its notification closed the owner.
    if (!paths.empty())
        (void)openPath(paths.front());
    if (owner && m_nav)
        m_nav->refresh();
    return paths;
}
} // namespace frost::app
