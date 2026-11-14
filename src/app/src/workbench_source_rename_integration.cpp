// SPDX-License-Identifier: Apache-2.0
// Prepare source/sidecar/project memberships on one worker, then admit one REF-1 operation.
#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_membership.hpp"
#include "frost/project/vcxproj_rename.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

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
namespace {
struct OpenRenameInput {
    std::string path;
    text::Snapshot text;
    bool dirty;
};
struct RenameProject {
    std::string path;
    std::string configuration;
    std::string platform;
};
} // namespace

Result<document::RefactoringPlan> Workbench::prepareProjectSourceRename(const std::string& oldPath,
                                                                        const std::string& newPath) {
    QPointer<Workbench> owner(this);
    const auto root = m_projectRoot;
    const auto solution = m_solution;
    const auto shownSolution = explorerSolutionModel();
    const auto evaluationGeneration = m_evaluatedGeneration;
    const auto trust = m_services.trust.snapshot(root);
    const auto active = m_build ? m_build->activeConfigurationIndex() : std::nullopt;
    const auto cmake = m_cmake ? m_cmake->sharedService() : nullptr;
    const auto cmakeModel = m_cmake ? m_cmake->shownModel() : nullptr;
    const auto profileGeneration = cmake ? cmake->profileGeneration() : 0;
    std::vector<std::pair<std::string, const document::FileDocument*>> documentLayout;
    for (const auto& document : m_documents->documents())
        documentLayout.emplace_back(document->path(), document.get());
    auto validate = [owner, root, solution, evaluationGeneration, trust, active, cmake, cmakeModel, profileGeneration,
                     documentLayout]() -> Result<void> {
        if (!owner || owner->m_shutDown)
            return fail(ErrorCode::Cancelled, "Workbench closed during source rename preparation");
        if (owner->m_projectRoot != root || owner->m_solution != solution ||
            owner->m_evaluatedGeneration != evaluationGeneration ||
            (owner->m_build ? owner->m_build->activeConfigurationIndex() : std::nullopt) != active)
            return fail(ErrorCode::Busy, "Project/configuration changed; prepare the rename again");
        const auto currentTrust = owner->m_services.trust.snapshot(root);
        if (currentTrust.trusted != trust.trusted || currentTrust.revision != trust.revision)
            return fail(ErrorCode::PermissionDenied, "Project trust changed during rename preparation");
        if (owner->m_buildService->state() != build::BuildState::Idle || owner->m_buildService->projectEditInProgress())
            return fail(ErrorCode::Busy, "Wait for the build/project edit before renaming a source");
        const auto documents = owner->m_documents->documents();
        if (documents.size() != documentLayout.size())
            return fail(ErrorCode::Busy, "Open documents changed during rename preparation");
        for (std::size_t i = 0; i < documents.size(); ++i) {
            if (documents[i].get() != documentLayout[i].second || documents[i]->path() != documentLayout[i].first)
                return fail(ErrorCode::Busy, "Open documents changed during rename preparation");
        }
        if (cmakeModel &&
            (!owner->m_cmake || owner->m_cmake->sharedService() != cmake || !owner->m_cmake->targetsCurrent() ||
             cmake->model() != cmakeModel || cmake->profileGeneration() != profileGeneration || cmake->modelLoading() ||
             cmake->modelStale() || cmake->state() != build::BuildState::Idle))
            return fail(ErrorCode::Busy, "CMake model/profile changed; configure and prepare the rename again");
        return {};
    };
    if (auto valid = validate(); !valid)
        return std::unexpected(valid.error());

    std::vector<OpenRenameInput> open;
    for (const auto& document : m_documents->documents()) {
        if (!document->untitled())
            open.push_back({document->path(), document->document().snapshot().text, document->dirty()});
    }
    std::vector<RenameProject> projects;
    if (shownSolution) {
        for (const auto& info : shownSolution->projects()) {
            if (info.kind != project::ProjectKind::Cpp || info.nodeKind != project::NodeKind::Project)
                continue;
            const auto id = shownSolution->findProjectByPath(info.path);
            if (!id)
                continue;
            const auto paths = shownSolution->projectFilePaths(*id);
            bool listed = std::ranges::any_of(
                paths, [&](const auto& path) { return lang::documentKey(path) == lang::documentKey(oldPath); });
            // Static project items include other configurations even when the current evaluated
            // hierarchy hides them. Do not lose another loaded project's shared membership.
            if (info.vcxproj)
                listed |= std::ranges::any_of(info.vcxproj->items, [&](const auto& item) {
                    return lang::documentKey(item.path) == lang::documentKey(oldPath);
                });
            if (!listed)
                continue;
            if (!active || *active >= info.configurationMap.size() || !info.configurationMap[*active])
                return fail(ErrorCode::Busy, "A source-owning project has no active configuration");
            const auto& mapping = *info.configurationMap[*active];
            projects.push_back({info.path, mapping.configuration, mapping.platform});
        }
    }
    std::optional<Error> hostBlocked;
    for (const auto& definition : projects) {
        if (auto blocked = projectEditBlocked(definition.path)) {
            hostBlocked = Error(ErrorCode::PermissionDenied, blocked->toStdString());
            break;
        }
    }
    auto host = m_options.projectEditHostPath;
    if (host.empty())
        host = QDir(QCoreApplication::applicationDirPath())
                   .filePath(QStringLiteral("project-host/ProjectHostVsProbe.exe"))
                   .toUtf8()
                   .toStdString();
    auto msbuild = m_options.msbuildPath.empty() ? m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath)
                                                 : m_options.msbuildPath;
    auto* fs = &m_services.fs;
    auto* launcher = &m_services.launcher;
    auto* trustStore = &m_services.trust;
    const auto label = "Rename " + std::string(io::fileName(oldPath));
    auto previews = std::make_shared<std::vector<project::CMakeMembershipPlan>>();
    auto prepared = prepareSourceRenameInBackground(
        m_window,
        [fs, launcher, trustStore, trust, oldPath, newPath, root, open = std::move(open),
         projects = std::move(projects), host = std::move(host), msbuild = std::move(msbuild), hostBlocked, cmakeModel,
         label, previews,
         hasSolution = bool(solution)](std::stop_token stop) mutable -> Result<document::RefactoringPlan> {
            TrustedProjectLauncher admitted(*launcher, *trustStore, root, trust.revision, stop);
            auto move = lang::planFmetaMove(*fs, oldPath, newPath);
            if (!move)
                return std::unexpected(move.error());
            auto plan = prepareSourceFileRename(*fs, *move, label);
            if (!plan)
                return plan;
            std::map<std::string, RefactoringInputSnapshot> inputs;
            auto capture = [&](const std::string& path) -> Result<RefactoringInputSnapshot> {
                const auto key = lang::documentKey(path);
                if (const auto it = inputs.find(key); it != inputs.end())
                    return it->second;
                const OpenRenameInput* document = nullptr;
                auto physical = fs->physicalPath(path);
                if (!physical && physical.error().code() != ErrorCode::NotFound)
                    return std::unexpected(physical.error());
                for (const auto& candidate : open) {
                    bool matches = lang::documentKey(candidate.path) == key;
                    if (physical) {
                        auto openPhysical = fs->physicalPath(candidate.path);
                        if (!openPhysical)
                            return fail(ErrorCode::Busy, "Cannot establish open input identity: " + candidate.path);
                        matches |= lang::documentKey(*openPhysical) == lang::documentKey(*physical);
                    }
                    if (!matches)
                        continue;
                    if (document)
                        return fail(ErrorCode::Busy, "Multiple open aliases refer to a project input: " + path);
                    document = &candidate;
                }
                auto input =
                    captureRefactoringInput(*fs, path, document ? std::optional(document->text.text()) : std::nullopt);
                if (input)
                    inputs.emplace(key, *input);
                return input;
            };
            if (!root.empty() && !cmakeModel && !hasSolution) {
                auto lists = io::joinPath(root, "CMakeLists.txt");
                if (!lists)
                    return std::unexpected(lists.error());
                auto input = capture(*lists);
                if (!input)
                    return std::unexpected(input.error());
                if (input->disk)
                    return fail(ErrorCode::Busy, "Configure CMake before preparing source membership changes");
                if (auto added = appendRefactoringDependency(*plan, *input); !added)
                    return std::unexpected(added.error());
            }
            if (cmakeModel) {
                std::vector<project::CMakeInputSnapshot> captured;
                for (const auto& file : cmakeModel->inputs) {
                    if (stop.stop_requested())
                        return fail(ErrorCode::Cancelled, "Rename preparation cancelled");
                    if (file.cmake)
                        continue;
                    const auto document = std::ranges::find_if(open, [&](const auto& item) {
                        return lang::documentKey(item.path) == lang::documentKey(file.path);
                    });
                    if (document != open.end() && document->dirty)
                        return fail(ErrorCode::Busy, "Save CMake inputs and configure before renaming a source");
                    auto input = capture(file.path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (!input->disk)
                        return fail(ErrorCode::NotFound, "A CMake input is missing: " + file.path);
                    if (auto matches = refactoringInputMatchesEvaluation(*input, input->disk); !matches)
                        return std::unexpected(matches.error());
                    if (auto added = appendRefactoringDependency(*plan, *input); !added)
                        return std::unexpected(added.error());
                    captured.push_back({file.path, *input->disk});
                }
                for (const auto& path : {cmakeModel->replyIndex, cmakeModel->buildDirectory + "/CMakeCache.txt"}) {
                    if (path.empty())
                        return fail(ErrorCode::Unsupported, "CMake reply identity is unavailable");
                    auto input = capture(path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (!input->disk)
                        return fail(ErrorCode::NotFound, "CMake reply/cache is missing");
                    if (auto added = appendRefactoringDependency(*plan, *input); !added)
                        return std::unexpected(added.error());
                }
                auto membership = project::planCMakeSourceRenameAllTargets(*cmakeModel, oldPath, newPath, captured);
                if (!membership)
                    return std::unexpected(membership.error());
                if (membership) {
                    for (const auto& edit : *membership) {
                        auto input = capture(edit.definitionPath);
                        if (!input)
                            return std::unexpected(input.error());
                        if (auto added = appendRefactoringReplacement(*plan, *input, edit.original, edit.replacement);
                            !added)
                            return std::unexpected(added.error());
                        previews->push_back(edit);
                    }
                }
            }
            for (const auto& definition : projects) {
                if (stop.stop_requested())
                    return fail(ErrorCode::Cancelled, "Rename preparation cancelled");
                std::vector<project::VcxprojRenameInput> captured;
                for (const auto& path : {definition.path, definition.path + ".filters"}) {
                    auto input = capture(path);
                    if (!input)
                        return std::unexpected(input.error());
                    captured.push_back({path, input->logical()});
                }
                auto membership = project::planVcxprojRename({definition.path, oldPath, newPath, captured});
                bool evaluatedByHost = false;
                if (!membership && membership.error().code() == ErrorCode::Unsupported) {
                    if (hostBlocked)
                        return std::unexpected(*hostBlocked);
                    if (msbuild.empty()) {
                        auto located = build::findMsBuild(admitted, *fs);
                        if (!located)
                            return std::unexpected(located.error());
                        msbuild = located->msbuildPath;
                    }
                    project::ProjectEdit edit;
                    edit.kind = project::ProjectEditKind::RenameFiles;
                    edit.root = root;
                    edit.definition = definition.path;
                    edit.files = {oldPath};
                    edit.renamedTo = {newPath};
                    edit.configuration = definition.configuration;
                    edit.platform = definition.platform;
                    edit.transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                    edit.commit = false;
                    edit.beforeFileMove = true;
                    auto hostInputs = captured;
                    for (auto& hostInput : hostInputs) {
                        const auto& original = inputs.at(lang::documentKey(hostInput.path));
                        if (refactoringInputMatchesEvaluation(original, original.disk))
                            hostInput.contents = original.disk;
                    }
                    membership = project::planVcxprojRenameWithHost(
                        admitted, {host, msbuild, std::chrono::seconds(120)}, edit, hostInputs, stop);
                    evaluatedByHost = true;
                }
                if (!membership)
                    return std::unexpected(membership.error());
                for (const auto& dependency : membership->dependencies) {
                    auto input = capture(dependency.path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (evaluatedByHost) {
                        if (auto matches = refactoringInputMatchesEvaluation(*input, dependency.contents); !matches)
                            return std::unexpected(matches.error());
                    } else if (input->logical() != dependency.contents) {
                        return fail(ErrorCode::Busy,
                                    "Parsed project input changed during preparation: " + dependency.path);
                    }
                    if (auto added = appendRefactoringDependency(*plan, *input); !added)
                        return std::unexpected(added.error());
                }
                for (const auto& edit : membership->replacements) {
                    auto input = capture(edit.path);
                    if (!input)
                        return std::unexpected(input.error());
                    if (auto added = appendRefactoringReplacement(*plan, *input, edit.before, edit.after); !added)
                        return std::unexpected(added.error());
                }
            }
            return plan;
        });
    if (!owner)
        return fail(ErrorCode::Cancelled, "Workbench closed during rename preparation");
    if (!prepared)
        return prepared;
    if (auto valid = validate(); !valid)
        return std::unexpected(valid.error());
    for (const auto& preview : *previews) {
        if (!m_services.prompts.confirmCMakeListsEdit(m_window, preview.definitionPath, preview.original,
                                                      preview.replacement))
            return fail(ErrorCode::Cancelled, "Project edit declined; no source file was moved");
        if (!owner)
            return fail(ErrorCode::Cancelled, "Workbench closed during project preview");
        if (auto valid = validate(); !valid)
            return std::unexpected(valid.error());
    }
    prepared->revalidateContext = std::move(validate);
    return prepared;
}

} // namespace frost::app
