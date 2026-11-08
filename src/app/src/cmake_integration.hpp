// SPDX-License-Identifier: Apache-2.0
// Per-folder CMake service, target tree, and build-tool-window integration.
#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/build/run_service.hpp"
#include "frost/core/error.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/cmake/cmake_controller.hpp"
#include "frost/ui/nav/session.hpp"

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

class QTreeView;
class QLabel;
class QAction;
class QComboBox;
class QLineEdit;

namespace frost::ui {
class BuildController;
class ShellWindow;
class RunConsole;
class RunToolWindow;
class BackgroundTask;
} // namespace frost::ui
namespace frost::build {
class BuildService;
}
namespace frost::settings {
class SettingsStore;
}
namespace frost::app {
class CMakeBuildBackend;

class CMakeIntegration final : public QObject {
public:
    // `runs` (the workbench's RunManager, RUN-1) starts the built target's program; it must outlive this.
    // `runWindow` (the Run tool window v2, RUN-3) gets one tab per target run; it must outlive this.
    CMakeIntegration(io::IFileSystem& fs, io::IProcessLauncher& launcher, project::TrustStore& trust,
                     settings::SettingsStore& settings, ui::BuildController& buildController,
                     build::BuildService& sharedAdmission, ui::ShellWindow& window, ui::RunToolWindow& runWindow,
                     run::RunManager& runs, std::string statePath, std::function<void()> runStateChanged,
                     std::function<void(const std::string&)> openFile, std::function<void()> modelChanged);
    ~CMakeIntegration() override;
    Result<void> open(const std::string& folder);
    void setTargetPreferences(std::vector<std::pair<std::string, std::string>> preferences,
                              std::function<void(const std::vector<std::pair<std::string, std::string>>&)> changed);
    void setArgumentPreferences(std::vector<ui::nav::SessionCMakeArguments> preferences,
                                std::function<void(const std::vector<ui::nav::SessionCMakeArguments>&)> changed);
    [[nodiscard]] build::CMakeService& service() const { return *m_service; }
    [[nodiscard]] std::shared_ptr<build::CMakeService> sharedService() const { return m_service; }
    [[nodiscard]] bool targetsCurrent() const {
        return m_controller->model() && m_controller->model() == m_service->model();
    }
    // The model the CMake Targets window and the target combo show (null before the first one):
    // what Run builds, so the run configurations are generated from it (RUN-2).
    [[nodiscard]] std::shared_ptr<const project::CMakeProject> shownModel() const { return m_controller->model(); }
    void filesChanged(const io::FileChangeBatch& batch) { m_service->filesChanged(batch); }
    [[nodiscard]] Result<build::RunId> run();
    [[nodiscard]] Result<void> debug(std::function<void(Result<debug::DebugTarget>)> done);
    [[nodiscard]] Result<build::RunId> rerun();
    [[nodiscard]] bool running() const { return m_pending || m_running; }
    void stop();
    void shutdown();
    void input(const QString& line);

    /*
     * RUN-2/RUN-3: the run configuration of a target run. Before the built program starts (or is
     * handed to the debugger), `configurator` gets the target's generated configuration (program,
     * arguments of the CMake Targets window, the source directory as working directory) and returns
     * what to start: the workbench lays the user's edits from Run/Debug Configurations over it.
     */
    void setLaunchConfigurator(std::function<run::RunConfiguration(run::RunConfiguration)> configurator) {
        m_launchConfigurator = std::move(configurator);
    }
    // Called with the Run/Debug target's name ("" for none) whenever the target list or choice is
    // refreshed, so the toolbar's configuration combo follows it.
    void setRunTargetObserver(std::function<void(const std::string& target)> observer) {
        m_runTargetObserver = std::move(observer);
    }
    // Makes the executable target `name` the Run/Debug target, as choosing it in the target combo
    // does; nothing when the model has no such executable target.
    void selectRunTarget(const std::string& name);
    // The Run/Debug target's name, or "" (no model, no executable target).
    [[nodiscard]] std::string runTargetName() const;

private:
    ui::BuildController& m_build;
    build::BuildService& m_sharedAdmission;
    std::shared_ptr<void> m_runAdmission;
    ui::ShellWindow& m_window;
    project::TrustStore& m_trust;
    settings::SettingsStore& m_settings;
    ui::RunToolWindow& m_runWindow;
    // The tab of the target run in progress (or the last one); null before the first run.
    QPointer<ui::RunConsole> m_console;
    // The console messages go to: the run's tab, else the Run tool window's current one.
    [[nodiscard]] ui::RunConsole& console();
    void finishConsole(const build::RunOutcome& outcome);
    run::RunManager& m_runs;
    std::function<run::RunConfiguration(run::RunConfiguration)> m_launchConfigurator;
    std::function<void(const std::string&)> m_runTargetObserver;
    std::function<void()> m_runStateChanged;
    std::unique_ptr<ui::cmake::CMakeController> m_controller;
    std::shared_ptr<build::CMakeService> m_service;
    std::unique_ptr<ui::cmake::CMakeServiceBackend> m_controllerBackend;
    std::unique_ptr<CMakeBuildBackend> m_buildBackend;
    QPointer<QTreeView> m_tree;
    QPointer<QLabel> m_targetLabel;
    QPointer<QLabel> m_guidanceLabel;
    QPointer<QComboBox> m_profiles;
    QPointer<QComboBox> m_targets;
    QPointer<QLineEdit> m_arguments;
    QPointer<QAction> m_solutionAction;
    QPointer<QAction> m_runAction;
    QPointer<QAction> m_profileAction;
    QPointer<QAction> m_targetAction;
    QPointer<QAction> m_wrapperAction;
    bool m_solutionActionVisible = false;
    bool m_runActionVisible = false;
    QStringList m_expandedKeys;
    std::vector<std::pair<std::string, std::string>> m_targetPreferences;
    std::function<void(const std::vector<std::pair<std::string, std::string>>&)> m_targetPreferencesChanged;
    std::vector<ui::nav::SessionCMakeArguments> m_argumentPreferences;
    std::function<void(const std::vector<ui::nav::SessionCMakeArguments>&)> m_argumentPreferencesChanged;
    bool m_restoringTarget = false;
    std::uint64_t m_displayProfileGeneration = 0;
    // CMK-15: the status bar task of the running configure (or reply read), cancelled through Stop.
    void refreshConfigureTask();
    std::shared_ptr<ui::BackgroundTask> m_configureTask;
    std::optional<std::stop_callback<std::function<void()>>> m_configureStop;
    struct RunRelay;
    std::shared_ptr<RunRelay> m_runRelay;
    // The target's program while it runs (RunManager's id).
    std::optional<run::RunId> m_runId;
    void runFinished(const run::RunResult& result);
    std::string m_runTarget;
    std::vector<std::string> m_runArguments;
    std::string m_runRoot;
    std::function<void(Result<debug::DebugTarget>)> m_debugDone;
    bool m_pending = false;
    bool m_running = false;
    bool m_rerun = false;
    bool m_stopRequested = false;
    bool m_shutdown = false;
    std::uint64_t m_runGeneration = 0;
    std::uint64_t m_targetGeneration = 0;
    std::uint64_t m_runTargetGeneration = 0;
    std::uint64_t m_profileGeneration = 0;
    void buildFinished(std::uint64_t generation, const build::CMakeBuildOutcome& outcome);
    void maybeRerun();
    [[nodiscard]] std::optional<std::size_t> runTarget(const project::CMakeProject& model) const;
    void refreshRunTarget();
    void restoreRunTarget();
    void rememberTreeTarget();
    void refreshProfiles();
    void editBuildWrapper();
};
} // namespace frost::app
