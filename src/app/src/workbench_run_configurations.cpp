// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): the run configurations of the open workspace (RUN-2/RUN-3).
// The generated ones come from the CMake File API model (one per executable target) and the open
// solution (one per C++ Application project); the user's edits and native configurations live in
// <workspace>/.frost/runConfigurations/configurations.json (RUN-1's store). A CMake target run lays
// the user's edits over the target's generated launch; a native run needs a trusted workspace.

#include "frost/app/workbench.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/core/log.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_types.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"

#include "cmake_integration.hpp"
#include <QDialog>

#include <format>
#include <utility>

namespace frost::app {
namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

} // namespace

std::vector<run::RunConfiguration> Workbench::generatedRunConfigurations() const {
    std::vector<run::RunConfiguration> generated;
    // CMake: the File API model the CMake Targets window shows (what Run builds; the service's newest
    // one may not have reached the window yet); a target runs in the source folder.
    if (m_cmake) {
        if (const auto model = m_cmake->shownModel())
            generated = run::cmakeRunConfigurations(*model, m_cmake->service().sourceDirectory());
    }
    // MSBuild: the solution's Application projects in the active solution configuration.
    if (m_solution) {
        std::optional<std::size_t> configuration;
        if (m_build) {
            const auto active = m_build->activeConfigurationIndex();
            const auto& names = m_build->configurations();
            if (active && *active < names.size()) {
                const auto all = m_solution->configurations();
                for (std::size_t i = 0; i < all.size(); ++i)
                    if (all[i].name() == names[*active].name())
                        configuration = i;
            }
        }
        for (run::RunConfiguration& entry : run::msbuildRunConfigurations(*m_solution, configuration)) {
            // A CMake target and a project of the same name keep distinct ids.
            entry.id = run::uniqueRunConfigurationId(entry.id, generated);
            generated.push_back(std::move(entry));
        }
    }
    return generated;
}

void Workbench::loadRunConfigurations() {
    if (!m_runController)
        return;
    std::vector<run::RunConfiguration> stored;
    m_runStoreUnreadable.clear();
    if (hasProject()) {
        auto path = run::workspaceRunConfigurationsPath(m_projectRoot);
        auto loaded = path ? run::loadRunConfigurations(m_services.fs, *path)
                           : Result<std::vector<run::RunConfiguration>>(std::unexpected(path.error()));
        if (loaded) {
            stored = std::move(*loaded);
        } else {
            // A store that cannot be read is left as it is: saveRunConfigurations() refuses until a
            // reopen reads it, so no edit replaces it with the (stored-empty) list in memory.
            m_runStoreUnreadable = loaded.error().message();
            log::warn(kLog, "run configurations: {}", loaded.error().describe());
            notify(text("Run configurations"), qs(loaded.error().message()), ui::NotificationKind::Error);
        }
    }
    m_runController->setStoredConfigurations(std::move(stored));
}

Result<void> Workbench::saveRunConfigurations(const std::vector<run::RunConfiguration>& stored) {
    if (!hasProject())
        return fail(ErrorCode::InvalidArgument, "run configurations need an open workspace");
    if (!m_runStoreUnreadable.empty())
        return fail(ErrorCode::Corrupt,
                    std::format("the saved run configurations could not be read when the folder opened ({}), so "
                                "they are not overwritten; fix or remove the file and reopen the folder",
                                m_runStoreUnreadable));
    return run::saveWorkspaceRunConfigurations(m_services.fs, m_projectRoot, stored);
}

run::RunConfiguration Workbench::configureCMakeLaunch(run::RunConfiguration launch) const {
    if (!m_runController)
        return launch;
    const run::RunConfiguration* edited = m_runController->configurations().find(launch.id);
    if (edited == nullptr || edited->type != run::RunType::CMakeTarget || edited->target != launch.target)
        return launch;
    // The user's edits win; empty arguments keep the CMake Targets window's per-profile arguments.
    launch.name = edited->name;
    if (!edited->arguments.empty())
        launch.arguments = edited->arguments;
    if (!edited->workingDirectory.empty())
        launch.workingDirectory = edited->workingDirectory;
    launch.environment = edited->environment;
    // RUN-5: the RunManager runs the configuration's before-launch tasks before the built program.
    launch.beforeLaunchTasks = edited->beforeLaunchTasks;
    return launch;
}

Result<void> Workbench::mayRunConfiguration(const run::RunConfiguration& configuration, std::function<void()> retry) {
    // A run configuration belongs to the open workspace, whose trust it needs (like a build).
    if (!hasProject() || m_services.trust.isTrusted(m_projectRoot))
        return {};
    if (m_window && m_build) {
        auto* dialog = new ui::TrustDialog(*m_build, {qs(m_projectRoot)}, m_window, ui::TrustPurpose::Build);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        const std::string id = configuration.id;
        const std::string root = m_projectRoot;
        connect(dialog, &QDialog::accepted, this, [this, id, root, retry = std::move(retry)] {
            // Retry only when the user trusted the workspace and it is still the open one.
            if (m_shutDown || !m_runController || root != m_projectRoot || !m_services.trust.isTrusted(root))
                return;
            if (retry) {
                retry();
                return;
            }
            if (auto started = m_runController->runConfiguration(id); !started)
                notify(text("Run"), qs(started.error().message()), ui::NotificationKind::Error);
        });
        dialog->open();
    }
    return fail(ErrorCode::Untrusted, "the workspace is not trusted; trust it to run its configurations");
}

void Workbench::connectCMakeRunConfigurations() {
    if (!m_cmake)
        return;
    m_cmake->setLaunchConfigurator(
        [this](run::RunConfiguration launch) { return configureCMakeLaunch(std::move(launch)); });
    /*
     * The target list and choice changed: regenerate, then let the combo follow the CMake target.
     * A target chosen in the combo while the window still showed an older model could not be made
     * the Run/Debug target then (selectCMakeTarget); it is applied now that the window caught up,
     * instead of following the old choice.
     */
    m_cmake->setRunTargetObserver([this](const std::string& target) {
        if (!m_runController || !m_cmake)
            return;
        m_runController->refreshStartupCombo();
        if (!m_pendingCMakeTarget.empty() && m_pendingCMakeTarget != target) {
            const std::string pending = std::exchange(m_pendingCMakeTarget, std::string());
            m_cmake->selectRunTarget(pending); // Calls this observer again once it applies.
            if (m_cmake && m_cmake->runTargetName() != pending)
                m_pendingCMakeTarget = pending; // Not in this model (yet).
            return;
        }
        m_pendingCMakeTarget.clear();
        m_runController->followCMakeTarget(target);
    });
}

} // namespace frost::app
