// SPDX-License-Identifier: Apache-2.0
/*
 * Workbench admission and lifecycle of project-definition edits (SLN-6, ADR 0082): adding,
 * removing and renaming items of a loaded C++ project's .vcxproj and .vcxproj.filters through the
 * shipped ProjectHost helper (the MSBuild construction API; frost/project/project_edit.hpp builds
 * the request), and recovering an interrupted edit from its journal. One edit runs at a time
 * (ProjectEditProcess); further ones queue behind it. Each edit reserves the build service, writes a
 * private recovery record before ProjectHost commits, and reloads the solution afterwards.
 */
#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/build/run_service.hpp"
#include "frost/document/file_document.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "managed_debug_preparation.hpp"
#include "project_edit_process.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QUuid>

#include <algorithm>

namespace frost::app {
namespace {
QString q(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// QFileDialog returns forward slashes on Windows, while the filesystem's canonical spelling uses
// backslashes. Change separators only: dot components stay, so the canonical comparison still
// refuses aliases and parent traversal.
std::string nativeSeparators(std::string path) {
#ifdef _WIN32
    std::ranges::replace(path, '/', '\\');
#endif
    return path;
}

// At most this many edits wait behind the running one (a move touching many projects).
constexpr std::size_t kMaxQueuedProjectEdits = 64;

// Awaiting retirement suspends this task, including on a one-worker pool. The Workbench retains
// and joins the resulting task before QObject destruction, so posting to its GUI context is safe.
sched::Task<void> projectEditReadersStopped(Workbench* owner, std::vector<sched::JoinHandle<void>> readers,
                                            std::function<void()> launch) {
    for (auto& reader : readers)
        co_await std::move(reader);
    QMetaObject::invokeMethod(owner, std::move(launch), Qt::QueuedConnection);
}
} // namespace

void Workbench::waitForProjectEditReaders(std::function<void()> launch) {
    m_projectEditWaitingForReaders = true;
    const auto generation = ++m_projectEditAdmissionGeneration;
    m_projectEvaluationDeferred = true;
    stopClangd();
    stopProjectEvaluation();
    auto readers = std::move(m_retiredProjectEvaluations);
    m_retiredProjectEvaluations.clear();
    for (auto& reader : m_retiredClangd)
        readers.push_back(std::move(reader));
    m_retiredClangd.clear();
    auto ready = [this, generation, launch = std::move(launch)]() mutable {
        if (m_shutDown || generation != m_projectEditAdmissionGeneration || !m_projectEditWaitingForReaders)
            return;
        m_projectEditWaitingForReaders = false;
        launch();
    };
    if (readers.empty()) {
        ready();
        return;
    }
    m_retiredProjectEvaluations.push_back(
        m_pool->spawn(sched::Lane::Background, projectEditReadersStopped(this, std::move(readers), std::move(ready))));
}

void Workbench::resumeProjectEvaluationAfterEdit() {
    if (!m_projectEvaluationDeferred || m_shutDown || m_projectRoot.empty() || m_buildService->projectEditInProgress())
        return;
    startProjectEvaluation(m_projectRoot);
    startClangd(m_projectRoot);
}

void Workbench::removeProjectFile(const QString& projectName, const QString& sourceName, bool adding,
                                  const QString& filter) {
    const QString title = adding ? QStringLiteral("Add Existing Source") : QStringLiteral("Remove from Project");
    // A pending journal is recovered first, whichever edit was asked for (nothing else may run).
    if (sourceName.isEmpty() || (m_solution && !m_projectRoot.empty() && projectEditJournalPresent())) {
        recoverProjectEdit(projectName);
        return;
    }
    project::ProjectEdit edit;
    edit.kind = adding ? project::ProjectEditKind::AddFiles : project::ProjectEditKind::RemoveFiles;
    edit.definition = utf8(projectName);
    edit.files = {nativeSeparators(utf8(sourceName))};
    edit.filter = adding ? utf8(filter) : std::string();
    if (auto refused = submitProjectEdit(std::move(edit), title))
        notify(title, *refused, ui::NotificationKind::Error);
}

void Workbench::addProjectFiles(const QString& projectName, const std::vector<std::string>& sources,
                                const QString& filter) {
    const QString title = QStringLiteral("Add to Project");
    project::ProjectEdit edit;
    edit.kind = project::ProjectEditKind::AddFiles;
    edit.definition = utf8(projectName);
    for (const std::string& source : sources)
        edit.files.push_back(nativeSeparators(source));
    edit.filter = utf8(filter);
    if (auto refused = submitProjectEdit(std::move(edit), title))
        notify(title, *refused, ui::NotificationKind::Error);
}

std::optional<QString> Workbench::projectEditBlocked(const std::string& projectPath) {
#ifndef _WIN32
    static_cast<void>(projectPath);
    return QStringLiteral("Project-definition edits are currently available on Windows only.");
#else
    const bool managedIdle =
        (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    if (!m_solution || !m_projectEdit || m_projectRoot.empty() || !m_services.trust.isTrusted(m_projectRoot) ||
        !m_services.trust.isTrusted(projectPath) || m_buildService->state() != build::BuildState::Idle ||
        anyDebugSessionActive() || !managedIdle)
        return QStringLiteral("Open a trusted solution and wait for any build or project edit to finish.");
    return std::nullopt;
#endif
}

Result<std::pair<std::string, std::string>> Workbench::projectEditTools() {
    std::string executable = m_options.projectEditHostPath;
    if (executable.empty())
        executable = utf8(QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("project-host/ProjectHostVsProbe.exe")));
    if (!io::isAbsolutePath(executable) || !m_services.fs.status(executable))
        return fail(ErrorCode::NotFound, "The ProjectHost helper is not installed.");
    std::string msbuild = m_options.msbuildPath.empty()
                              ? m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath)
                              : m_options.msbuildPath;
    if (msbuild.empty()) {
        auto found = build::findMsBuild(m_services.launcher, m_services.fs);
        if (!found)
            return std::unexpected(std::move(found.error()));
        msbuild = found->msbuildPath;
    }
    if (!io::isAbsolutePath(msbuild) || m_options.projectEditStateDirectory.empty())
        return fail(ErrorCode::NotFound, "ProjectHost or private recovery storage is unavailable.");
    if (auto made = m_services.fs.createDirectories(m_options.projectEditStateDirectory); !made)
        return std::unexpected(std::move(made.error()));
    return std::pair{std::move(executable), std::move(msbuild)};
}

bool Workbench::projectEditJournalPresent() {
    const auto journal = io::joinPath(m_projectRoot, ".frost-project-edit-journal");
    if (!journal)
        return true;
    const auto exists = m_services.fs.exists(*journal);
    return !exists || *exists;
}

void Workbench::publishProjectEdit(const std::shared_ptr<const project::SolutionModel>& solution,
                                   const QString& title) {
    auto loaded = project::SolutionModel::load(m_services.fs, solution->path(), {.cppHierarchy = m_cppHierarchyMode});
    if (!loaded) {
        notify(title, q(loaded.error().message()), ui::NotificationKind::Error);
        return;
    }
    auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    if (m_solution != solution)
        return;
    if (auto updated = m_buildService->finishProjectEdit(refreshed); !updated) {
        notify(title, q(updated.error().message()), ui::NotificationKind::Error);
        return;
    }
    m_solution = refreshed;
    m_build->projectEditPublished();
    if (auto updated = m_runService->openSolution(refreshed); !updated)
        notify(title, q(updated.error().message()), ui::NotificationKind::Warning);
    if (m_nav)
        static_cast<void>(m_nav->refreshSolution(*refreshed));
}

void Workbench::startQueuedProjectEdit() {
    while (!m_projectEditQueue.empty() && !m_shutDown && !m_projectEditWaitingForReaders && m_projectEdit &&
           !m_projectEdit->busy()) {
        auto [edit, title] = std::move(m_projectEditQueue.front());
        m_projectEditQueue.pop_front();
        if (auto refused = submitProjectEdit(std::move(edit), title))
            notify(title, *refused + QStringLiteral(" No project file was changed."), ui::NotificationKind::Warning);
    }
}

std::optional<QString> Workbench::submitProjectEdit(project::ProjectEdit edit, const QString& title) {
    if (m_projectEdit && (m_projectEditWaitingForReaders || m_projectEdit->busy()) && m_solution) {
        if (m_projectEditQueue.size() >= kMaxQueuedProjectEdits)
            return QStringLiteral("Too many project edits are waiting.");
        m_projectEditQueue.emplace_back(std::move(edit), title);
        return std::nullopt;
    }
    const std::string projectPath = edit.definition;
    if (auto blocked = projectEditBlocked(projectPath))
        return blocked;
    const auto solution = m_solution;
    const std::string root = m_projectRoot;
    const auto projectIt = std::ranges::find_if(solution->projects(), [&](const auto& item) {
        return lang::documentKey(item.path) == lang::documentKey(projectPath);
    });
    if (projectIt == solution->projects().end() || !projectIt->vcxproj || edit.files.empty())
        return QStringLiteral("The file does not belong to a loaded C++ project.");
    const auto listed = [&](const std::string& path) {
        return std::ranges::any_of(projectIt->vcxproj->items, [&](const project::ProjectItem& item) {
            return !item.fromWildcard && lang::documentKey(item.path) == lang::documentKey(path);
        });
    };
    const std::string projectDirectory(io::parentPath(projectPath));
    // A file the edit brings in: an existing regular file inside the project directory, spelled
    // canonically (no aliases), and trusted.
    const auto incomingOk = [&](const std::string& path) {
        const auto canonical = m_services.fs.canonicalPath(path);
        const auto status = m_services.fs.status(path);
        return canonical && status && status->type == io::FileType::Regular && *canonical == path &&
               path.starts_with(projectDirectory) && path.size() > projectDirectory.size() &&
               (path[projectDirectory.size()] == '/' || path[projectDirectory.size()] == '\\') &&
               m_services.trust.isTrusted(path);
    };
    if (edit.kind == project::ProjectEditKind::AddFiles) {
        const auto canonicalProject = m_services.fs.canonicalPath(projectPath);
        if (!canonicalProject || *canonicalProject != projectPath || !std::ranges::all_of(edit.files, incomingOk))
            return QStringLiteral("Choose an existing file inside the trusted project directory without aliases.");
    } else if (edit.kind == project::ProjectEditKind::RemoveFiles ||
               edit.kind == project::ProjectEditKind::RenameFiles) {
        if (!std::ranges::all_of(edit.files, listed))
            return QStringLiteral("Only items the project lists directly, not through a wildcard, can be removed or "
                                  "renamed.");
        if (edit.kind == project::ProjectEditKind::RenameFiles &&
            (edit.renamedTo.size() != edit.files.size() ||
             !std::ranges::all_of(edit.renamedTo, [&](const auto& path) { return m_services.trust.isTrusted(path); })))
            return QStringLiteral("The renamed files are not inside the trusted solution.");
    } else {
        return QStringLiteral("Only file edits of a C++ project are available here.");
    }
    const auto active = m_build->activeConfigurationIndex();
    if (!active || *active >= projectIt->configurationMap.size() || !projectIt->configurationMap[*active])
        return QStringLiteral("The project has no active configuration.");
    edit.configuration = projectIt->configurationMap[*active]->configuration;
    edit.platform = projectIt->configurationMap[*active]->platform;
    const std::string filters = projectPath + ".filters";
    const auto definitionsClosed = [this, projectPath, filters] {
        return !m_documentRegistry->find(projectPath) && !m_documentRegistry->find(filters);
    };
    if (!definitionsClosed())
        return QStringLiteral("Close the project and filters definition tabs before editing the project.");
    auto tools = projectEditTools();
    if (!tools)
        return q(tools.error().message());
    if (projectEditJournalPresent())
        return QStringLiteral("An interrupted project edit must be recovered first (Recover in the project's menu).");
    const std::string id = utf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto marker = io::joinPath(m_options.projectEditStateDirectory, id + ".json");
    if (!marker)
        return q(marker.error().message());
    edit.root = root;
    edit.transactionId = id;
    edit.commit = true;
    auto request = project::projectEditRequest(edit);
    if (!request)
        return q(request.error().message());
    QJsonObject lineage{{"Root", q(root)}, {"Project", q(projectPath)}, {"TransactionId", q(id)}};
    io::ProcessSpec spec;
    spec.executable = tools->first;
    spec.arguments = {std::string(io::parentPath(tools->second))};
    if (auto reserved = m_buildService->beginProjectEdit(); !reserved)
        return q(reserved.error().message());
    const auto trustRevision = m_services.trust.snapshot(root).revision;
    const auto managedIdle = [this] {
        return (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    const std::string& markerPath = *marker;
    const auto admissible = [this, solution, root, projectPath, definitionsClosed, managedIdle, trustRevision] {
        const auto trust = m_services.trust.snapshot(root);
        return !m_shutDown && m_solution == solution && m_projectRoot == root && trust.trusted &&
               trust.revision == trustRevision && m_services.trust.isTrusted(projectPath) &&
               m_buildService->state() == build::BuildState::Idle && definitionsClosed() && !anyDebugSessionActive() &&
               managedIdle();
    };
    const auto admit = [this, admissible, markerPath, lineage](const std::string& digest) mutable {
        if (!admissible())
            return false;
        const auto exists = m_services.fs.exists(markerPath);
        if (!exists || *exists)
            return false;
        lineage.insert("JournalDigest", q(digest));
        const auto bytes = QJsonDocument(lineage).toJson(QJsonDocument::Compact);
        const bool saved = m_services.fs
                               .writeFileAtomic(markerPath, std::string_view(bytes.constData(),
                                                                             static_cast<std::size_t>(bytes.size())))
                               .has_value();
        if (!saved)
            return false;
        if (!admissible()) {
            static_cast<void>(m_services.fs.removeFile(markerPath));
            return false;
        }
        return true;
    };
    const auto complete = [this, solution, root, markerPath, title](bool success, bool accepted,
                                                                    const std::string& error) {
        if (success || !accepted)
            static_cast<void>(m_services.fs.removeFile(markerPath));
        if (!success) {
            if (!accepted)
                m_buildService->cancelProjectEdit();
            // Queued edits were planned against the state this one failed to change.
            m_projectEditQueue.clear();
            notify(title, q(error + (accepted ? " Recovery information was retained at " + markerPath : "")),
                   ui::NotificationKind::Error);
            resumeProjectEvaluationAfterEdit();
            return;
        }
        if (m_solution != solution || m_projectRoot != root) {
            m_projectEditQueue.clear();
            return;
        }
        publishProjectEdit(solution, title);
        startQueuedProjectEdit();
        resumeProjectEvaluationAfterEdit();
    };
    waitForProjectEditReaders(
        [this, admissible, spec = std::move(spec), request = std::move(*request), id, admit, complete]() mutable {
            if (!admissible()) {
                complete(false, false, "The project or its trust changed while project readers were stopping.");
                return;
            }
            auto started = m_projectEdit->start(std::move(spec), std::move(request), id, admit, complete);
            if (!started)
                complete(false, false, started.error().message());
        });
    return std::nullopt;
}

void Workbench::recoverProjectEdit(const QString& projectName) {
    const QString title = QStringLiteral("Project recovery");
    const auto failEdit = [this, title](const QString& reason) { notify(title, reason, ui::NotificationKind::Error); };
    const std::string projectPath = utf8(projectName);
    if (auto blocked = projectEditBlocked(projectPath);
        blocked || m_projectEditWaitingForReaders || m_projectEdit->busy()) {
        failEdit(blocked.value_or(QStringLiteral("Wait for the running project edit to finish.")));
        return;
    }
    const auto solution = m_solution;
    const std::string root = m_projectRoot;
    const auto managedIdle = [this] {
        return (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    const auto projectIt = std::ranges::find_if(solution->projects(), [&](const auto& item) {
        return lang::documentKey(item.path) == lang::documentKey(projectPath);
    });
    if (projectIt == solution->projects().end() || !projectIt->vcxproj) {
        failEdit(QStringLiteral("The source does not belong to a loaded C++ project."));
        return;
    }
    const std::string filters = projectPath + ".filters";
    const auto definitionsClosed = [this, projectPath, filters] {
        return !m_documentRegistry->find(projectPath) && !m_documentRegistry->find(filters);
    };
    if (!definitionsClosed()) {
        failEdit(QStringLiteral("Close the project and filters definition tabs before editing the project."));
        return;
    }
    auto tools = projectEditTools();
    if (!tools) {
        failEdit(q(tools.error().message()));
        return;
    }
    const auto journal = io::joinPath(root, ".frost-project-edit-journal");
    if (!journal) {
        failEdit(q(journal.error().message()));
        return;
    }
    const auto journalExists = m_services.fs.exists(*journal);
    if (!journalExists) {
        failEdit(q(journalExists.error().message()));
        return;
    }
    if (!*journalExists) {
        failEdit(QStringLiteral("There is no pending project edit journal."));
        return;
    }
    const auto entries = m_services.fs.listDirectory(m_options.projectEditStateDirectory);
    if (!entries) {
        failEdit(q(entries.error().message()));
        return;
    }
    std::string ownedMarker, ownedBytes, ownedId, ownedDigest;
    for (const auto& entry : *entries) {
        if (entry.type != io::FileType::Regular || !entry.name.ends_with(".json"))
            continue;
        const auto marker = io::joinPath(m_options.projectEditStateDirectory, entry.name);
        if (!marker)
            continue;
        const auto status = m_services.fs.status(*marker);
        if (!status || status->size > 16384)
            continue;
        const auto bytes = m_services.fs.readFile(*marker);
        if (!bytes)
            continue;
        const auto json = QJsonDocument::fromJson(QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())));
        if (!json.isObject())
            continue;
        const auto object = json.object();
        const std::string markerRoot = utf8(object.value("Root").toString());
        const std::string markerProject = utf8(object.value("Project").toString());
        const std::string id = utf8(object.value("TransactionId").toString());
        const std::string digest = utf8(object.value("JournalDigest").toString());
        if (lang::documentKey(markerRoot) != lang::documentKey(root) ||
            lang::documentKey(markerProject) != lang::documentKey(projectPath) || entry.name != id + ".json" ||
            QUuid(q(id)).isNull() || digest.size() != 64 ||
            digest.find_first_not_of("0123456789ABCDEF") != std::string::npos)
            continue;
        if (!ownedMarker.empty()) {
            failEdit(QStringLiteral("Multiple recovery records match this project."));
            return;
        }
        ownedMarker = *marker;
        ownedBytes = *bytes;
        ownedId = id;
        ownedDigest = digest;
    }
    if (ownedMarker.empty()) {
        failEdit(QStringLiteral(
            "The project has a journal without a matching private recovery record. No recovery was attempted."));
        return;
    }
    if (!m_services.prompts.confirmProjectRecovery(m_window, projectName))
        return;
    if (!managedIdle()) {
        failEdit(QStringLiteral("Wait for the pending debug launch to finish."));
        return;
    }
    if (!m_buildService->projectEditInProgress()) {
        if (auto reserved = m_buildService->beginProjectEdit(); !reserved) {
            failEdit(q(reserved.error().message()));
            return;
        }
    }
    const auto trustRevision = m_services.trust.snapshot(root).revision;
    QJsonObject request{{"Project", projectName},      {"Root", q(root)},
                        {"Trusted", "true"},           {"Recover", "true"},
                        {"TransactionId", q(ownedId)}, {"JournalDigest", q(ownedDigest)}};
    io::ProcessSpec spec;
    spec.executable = tools->first;
    spec.arguments = {std::string(io::parentPath(tools->second))};
    const auto admit = [this, solution, root, projectPath, definitionsClosed, managedIdle, ownedMarker, ownedBytes,
                        ownedDigest, trustRevision](const std::string& digest) {
        if (m_shutDown || m_solution != solution || m_projectRoot != root || !m_services.trust.isTrusted(root) ||
            m_services.trust.snapshot(root).revision != trustRevision || !m_services.trust.isTrusted(projectPath) ||
            !definitionsClosed() || digest != ownedDigest || anyDebugSessionActive() || !managedIdle())
            return false;
        const auto bytes = m_services.fs.readFile(ownedMarker);
        return bytes && *bytes == ownedBytes;
    };
    const auto complete = [this, solution, root, ownedMarker, title](bool success, bool, const std::string& error) {
        if (!success) {
            notify(title, q(error), ui::NotificationKind::Error);
            return;
        }
        static_cast<void>(m_services.fs.removeFile(ownedMarker));
        if (m_solution != solution || m_projectRoot != root)
            return;
        publishProjectEdit(solution, title);
        resumeProjectEvaluationAfterEdit();
    };
    waitForProjectEditReaders([this, spec = std::move(spec),
                               request = QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString(), ownedId,
                               ownedDigest, admit, complete, failEdit]() mutable {
        if (!admit(ownedDigest)) {
            failEdit(QStringLiteral("The project or its trust changed while project readers were stopping."));
            return;
        }
        auto started = m_projectEdit->start(std::move(spec), std::move(request), ownedId, admit, complete);
        if (!started)
            failEdit(q(started.error().message()));
    });
}
} // namespace frost::app
