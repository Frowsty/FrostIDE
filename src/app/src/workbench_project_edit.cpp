// SPDX-License-Identifier: Apache-2.0
// Workbench admission and lifecycle for bounded MSBuild source removal.
#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/build/run_service.hpp"
#include "frost/document/file_document.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "managed_debug_preparation.hpp"
#include "project_edit_process.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace frost::app {
namespace {
QString q(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
} // namespace

void Workbench::removeProjectFile(const QString& projectName, const QString& sourceName) {
    const auto failEdit = [this](const QString& reason) {
        notify(ui::qtr("frost::app::Workbench", "Remove from Project"), reason, ui::NotificationKind::Error);
    };
#ifndef _WIN32
    failEdit(QStringLiteral("Project-definition removal is currently available on Windows only."));
    return;
#endif
    const std::string projectPath = utf8(projectName), sourcePath = utf8(sourceName);
    const auto solution = m_solution;
    const std::string root = m_projectRoot;
    const auto managedIdle = [this] {
        return (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    if (!solution || !m_projectEdit || m_projectEdit->busy() || root.empty() || !m_services.trust.isTrusted(root) ||
        !m_services.trust.isTrusted(projectPath) || m_buildService->state() != build::BuildState::Idle ||
        (m_debug && m_debug->debugging()) || !managedIdle()) {
        failEdit(QStringLiteral("Open a trusted solution and wait for any build or project edit to finish."));
        return;
    }
    const auto projectIt = std::ranges::find_if(solution->projects(), [&](const auto& item) {
        return lang::documentKey(item.path) == lang::documentKey(projectPath);
    });
    if (projectIt == solution->projects().end() || !projectIt->vcxproj) {
        failEdit(QStringLiteral("The source does not belong to a loaded C++ project."));
        return;
    }
    const auto item = std::ranges::find_if(projectIt->vcxproj->items, [&](const auto& entry) {
        return lang::documentKey(entry.path) == lang::documentKey(sourcePath);
    });
    if (!sourceName.isEmpty() &&
        (item == projectIt->vcxproj->items.end() || item->type != project::ItemType::ClCompile || item->fromWildcard)) {
        failEdit(QStringLiteral("Only directly included C++ source items are supported."));
        return;
    }
    const auto active = m_build->activeConfigurationIndex();
    if (!sourceName.isEmpty() &&
        (!active || *active >= projectIt->configurationMap.size() || !projectIt->configurationMap[*active])) {
        failEdit(QStringLiteral("The project has no active configuration."));
        return;
    }
    const auto configuration =
        sourceName.isEmpty() ? project::ProjectConfigurationMapping{} : *projectIt->configurationMap[*active];
    const std::string filters = projectPath + ".filters";
    const auto definitionsClosed = [this, projectPath, filters] {
        return !m_documentRegistry->find(projectPath) && !m_documentRegistry->find(filters);
    };
    if (!definitionsClosed()) {
        failEdit(QStringLiteral("Close the project and filters definition tabs before editing the project."));
        return;
    }
    std::string executable = m_options.projectEditHostPath;
    if (executable.empty())
        executable = utf8(QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("project-host/ProjectHostVsProbe.exe")));
    if (!io::isAbsolutePath(executable) || !m_services.fs.status(executable)) {
        failEdit(QStringLiteral("The ProjectHost helper is not installed."));
        return;
    }
    std::string msbuild = m_options.msbuildPath.empty()
                              ? m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath)
                              : m_options.msbuildPath;
    if (msbuild.empty()) {
        auto found = build::findMsBuild(m_services.launcher, m_services.fs);
        if (!found) {
            failEdit(q(found.error().message()));
            return;
        }
        msbuild = found->msbuildPath;
    }
    if (!io::isAbsolutePath(msbuild) || m_options.projectEditStateDirectory.empty()) {
        failEdit(QStringLiteral("ProjectHost or private recovery storage is unavailable."));
        return;
    }
    if (auto made = m_services.fs.createDirectories(m_options.projectEditStateDirectory); !made) {
        failEdit(q(made.error().message()));
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
    if (*journalExists) {
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
        if (!m_buildService->projectEditInProgress()) {
            if (auto reserved = m_buildService->beginProjectEdit(); !reserved) {
                failEdit(q(reserved.error().message()));
                return;
            }
        }
        QJsonObject request{{"Project", projectName},      {"Root", q(root)},
                            {"Trusted", "true"},           {"Recover", "true"},
                            {"TransactionId", q(ownedId)}, {"JournalDigest", q(ownedDigest)}};
        io::ProcessSpec spec;
        spec.executable = executable;
        spec.arguments = {std::string(io::parentPath(msbuild))};
        const auto admit = [this, solution, root, projectPath, definitionsClosed, managedIdle, ownedMarker, ownedBytes,
                            ownedDigest](const std::string& digest) {
            if (m_shutDown || m_solution != solution || m_projectRoot != root || !m_services.trust.isTrusted(root) ||
                !m_services.trust.isTrusted(projectPath) || !definitionsClosed() || digest != ownedDigest ||
                (m_debug && m_debug->debugging()) || !managedIdle())
                return false;
            const auto bytes = m_services.fs.readFile(ownedMarker);
            return bytes && *bytes == ownedBytes;
        };
        const auto complete = [this, solution, root, ownedMarker](bool success, bool, const std::string& error) {
            if (!success) {
                notify(QStringLiteral("Project recovery"), q(error), ui::NotificationKind::Error);
                return;
            }
            static_cast<void>(m_services.fs.removeFile(ownedMarker));
            auto loaded = project::SolutionModel::load(m_services.fs, solution->path());
            if (!loaded) {
                notify(QStringLiteral("Project recovery"), q(loaded.error().message()), ui::NotificationKind::Error);
                return;
            }
            auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
            if (m_solution != solution || m_projectRoot != root)
                return;
            if (auto updated = m_buildService->finishProjectEdit(refreshed); !updated) {
                notify(QStringLiteral("Project recovery"), q(updated.error().message()), ui::NotificationKind::Error);
                return;
            }
            m_solution = refreshed;
            m_build->projectEditPublished();
            if (auto updated = m_runService->openSolution(refreshed); !updated)
                notify(QStringLiteral("Project recovery"), q(updated.error().message()), ui::NotificationKind::Warning);
            if (m_nav)
                static_cast<void>(m_nav->refreshSolution(*refreshed));
        };
        auto started =
            m_projectEdit->start(std::move(spec), QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString(),
                                 ownedId, admit, complete);
        if (!started)
            failEdit(q(started.error().message()));
        return;
    }
    if (sourceName.isEmpty()) {
        failEdit(QStringLiteral("There is no pending project edit journal."));
        return;
    }
    const std::string id = utf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto marker = io::joinPath(m_options.projectEditStateDirectory, id + ".json");
    if (!marker) {
        failEdit(q(marker.error().message()));
        return;
    }
    QJsonObject request{{"Project", projectName},
                        {"Root", q(root)},
                        {"Trusted", "true"},
                        {"Configuration", q(configuration.configuration)},
                        {"Platform", q(configuration.platform)},
                        {"Remove", q(item->include)},
                        {"Commit", "true"},
                        {"TransactionId", q(id)}};
    QJsonObject lineage{{"Root", q(root)}, {"Project", projectName}, {"TransactionId", q(id)}};
    io::ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {std::string(io::parentPath(msbuild))};
    if (auto reserved = m_buildService->beginProjectEdit(); !reserved) {
        failEdit(q(reserved.error().message()));
        return;
    }
    const std::string markerPath = *marker;
    const auto admit = [this, solution, root, projectPath, definitionsClosed, managedIdle, markerPath,
                        lineage](const std::string& digest) mutable {
        if (m_shutDown || m_solution != solution || m_projectRoot != root || !m_services.trust.isTrusted(root) ||
            !m_services.trust.isTrusted(projectPath) || m_buildService->state() != build::BuildState::Idle ||
            !definitionsClosed() || (m_debug && m_debug->debugging()) || !managedIdle())
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
        if (m_shutDown || m_solution != solution || m_projectRoot != root || !m_services.trust.isTrusted(root) ||
            !m_services.trust.isTrusted(projectPath) || m_buildService->state() != build::BuildState::Idle ||
            !definitionsClosed() || (m_debug && m_debug->debugging()) || !managedIdle()) {
            static_cast<void>(m_services.fs.removeFile(markerPath));
            return false;
        }
        return true;
    };
    const auto complete = [this, solution, root, markerPath](bool success, bool accepted, const std::string& error) {
        if (success || !accepted)
            static_cast<void>(m_services.fs.removeFile(markerPath));
        if (!success) {
            if (!accepted)
                m_buildService->cancelProjectEdit();
            notify(QStringLiteral("Remove from Project"),
                   q(error + (accepted ? " Recovery information was retained at " + markerPath : "")),
                   ui::NotificationKind::Error);
            return;
        }
        if (m_solution != solution || m_projectRoot != root) {
            return;
        }
        auto loaded = project::SolutionModel::load(m_services.fs, solution->path());
        if (!loaded) {
            notify(QStringLiteral("Remove from Project"), q(loaded.error().message()), ui::NotificationKind::Error);
            return;
        }
        auto refreshed = std::make_shared<const project::SolutionModel>(std::move(*loaded));
        if (auto updated = m_buildService->finishProjectEdit(refreshed); !updated) {
            notify(QStringLiteral("Remove from Project"), q(updated.error().message()), ui::NotificationKind::Error);
            return;
        }
        m_solution = refreshed;
        m_build->projectEditPublished();
        if (auto updated = m_runService->openSolution(refreshed); !updated)
            notify(QStringLiteral("Remove from Project"), q(updated.error().message()), ui::NotificationKind::Warning);
        if (m_nav)
            static_cast<void>(m_nav->refreshSolution(*refreshed));
    };
    auto started = m_projectEdit->start(
        std::move(spec), QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString(), id, admit, complete);
    if (!started) {
        m_buildService->cancelProjectEdit();
        failEdit(q(started.error().message()));
    }
}
} // namespace frost::app
