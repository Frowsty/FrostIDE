// SPDX-License-Identifier: Apache-2.0
// Trust-scoped visualizer discovery runs on a joined worker before native launch.
#include "frost/app/workbench.hpp"
#include "frost/debug/natvis_discovery.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"

#include "cmake_integration.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProgressDialog>
#include <QStandardPaths>

#include <optional>
#include <set>
#include <thread>

namespace frost::app {
Result<std::vector<std::string>> Workbench::discoverProjectVisualizers(const debug::DebugTarget&) {
    const auto root = m_projectRoot;
    const auto trust = m_services.trust.snapshot(root);
    if (!trust.trusted)
        return fail(ErrorCode::Untrusted, "Trust the project before loading visualizers");
    QPointer<Workbench> owner(this);
    const auto solution = explorerSolutionModel();
    const auto model = m_cmake ? m_cmake->shownModel() : nullptr;
    debug::NatvisDiscovery request;
    std::set<std::string> directories{root};
    auto addFile = [&](const std::string& path) {
        if (QString::fromStdString(path).endsWith(QStringLiteral(".natvis"), Qt::CaseInsensitive))
            request.files.push_back(path);
    };
    // Authoritative project items include imported and dependency visualizers without attributing
    // arbitrary filesystem files to compilation targets.
    if (solution)
        for (const auto& project : solution->projects()) {
            if (project.nodeKind != frost::project::NodeKind::Project || !io::isAbsolutePath(project.path))
                continue;
            directories.insert(std::string(io::parentPath(project.path)));
            if (const auto id = solution->findProjectByPath(project.path))
                for (const auto& file : solution->projectFilePaths(*id))
                    addFile(file);
            if (project.vcxproj)
                for (const auto& item : project.vcxproj->items)
                    addFile(item.path);
        }
    if (model)
        for (const auto& target : model->targets) {
            if (!target.sourceDirectory.empty())
                directories.insert(target.sourceDirectory);
            for (const auto& source : target.sources)
                addFile(source.path);
        }
    request.directories.assign(directories.begin(), directories.end());
    request.roots = {root + "/visualizers", root + "/.frost/visualizers",
                     QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toUtf8().toStdString() +
                         "/visualizers",
                     QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toUtf8().toStdString() +
                         "/Visual Studio 2022/Visualizers"};
    request.files.push_back(
        QDir(QCoreApplication::applicationDirPath()).filePath("visualizers/FrostCommon.natvis").toUtf8().toStdString());
    // Use only the configured local Build Tools installation. Never scan arbitrary PATH or ship
    // Microsoft's files. Missing installed STL visualizers produce a visible diagnostic.
    const auto msbuild = m_options.msbuildPath.empty()
                             ? m_services.settings.snapshot()->get(settings::key::toolsMsbuildPath)
                             : m_options.msbuildPath;
    if (!msbuild.empty()) {
        QDir installation(QFileInfo(QString::fromStdString(msbuild)).absolutePath());
        // Build Tools installs both Bin/MSBuild.exe and Bin/amd64/MSBuild.exe. Search only
        // the configured installation's bounded ancestors; never another installation or PATH.
        for (int level = 0; level != 6; ++level) {
            const auto candidate = installation.filePath("Common7/Packages/Debugger/Visualizers/stl.natvis");
            if (QFileInfo::exists(candidate)) {
                request.files.push_back(candidate.toUtf8().toStdString());
                break;
            }
            if (!installation.cdUp())
                break;
        }
    }
    QProgressDialog progress(QStringLiteral("Discovering native visualizers…"), QString(), 0, 0);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    std::optional<Result<debug::NatvisInventory>> result;
    auto* fs = &m_services.fs;
    std::jthread worker([&] {
        auto inventory = debug::discoverNatvis(*fs, request);
        QMetaObject::invokeMethod(
            &progress,
            [&, inventory = std::move(inventory)]() mutable {
                result = std::move(inventory);
                progress.done(0);
            },
            Qt::QueuedConnection);
    });
    progress.exec();
    worker.join();
    if (!owner)
        return fail(ErrorCode::Cancelled, "Workbench closed during visualizer discovery");
    if (!result)
        return fail(ErrorCode::Cancelled, "Visualizer discovery interrupted");
    if (!*result)
        return std::unexpected(result->error());
    const auto now = m_services.trust.snapshot(root);
    if (m_projectRoot != root || !now.trusted || now.revision != trust.revision ||
        explorerSolutionModel() != solution || (m_cmake ? m_cmake->shownModel() : nullptr) != model)
        return fail(ErrorCode::Busy, "Project/trust changed during visualizer discovery");
    for (const auto& diagnostic : (**result).diagnostics)
        notify(QStringLiteral("Native visualizers"), QString::fromStdString(diagnostic), ui::NotificationKind::Warning);
    return std::move((**result).files);
}
} // namespace frost::app
