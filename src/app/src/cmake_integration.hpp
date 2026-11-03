// SPDX-License-Identifier: Apache-2.0
// Per-folder CMake service, target tree, and build-tool-window integration.
#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/build/run_service.hpp"
#include "frost/build/run_session.hpp"
#include "frost/core/error.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/ui/cmake/cmake_controller.hpp"

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QTreeView;
class QLabel;
class QAction;
class QComboBox;

namespace frost::ui {
class BuildController;
class ShellWindow;
class RunConsole;
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
    CMakeIntegration(io::IFileSystem& fs, io::IProcessLauncher& launcher, project::TrustStore& trust,
                     settings::SettingsStore& settings, ui::BuildController& buildController,
                     build::BuildService& sharedAdmission, ui::ShellWindow& window, ui::RunConsole& console,
                     std::string statePath, std::function<void()> runStateChanged,
                     std::function<void(const std::string&)> openFile, std::function<void()> modelChanged);
    ~CMakeIntegration() override;
    Result<void> open(const std::string& folder);
    void setTargetPreferences(std::vector<std::pair<std::string, std::string>> preferences,
                              std::function<void(const std::vector<std::pair<std::string, std::string>>&)> changed);
    [[nodiscard]] build::CMakeService& service() const { return *m_service; }
    [[nodiscard]] std::shared_ptr<build::CMakeService> sharedService() const { return m_service; }
    [[nodiscard]] bool targetsCurrent() const {
        return m_controller->model() && m_controller->model() == m_service->model();
    }
    void filesChanged(const io::FileChangeBatch& batch) { m_service->filesChanged(batch); }
    [[nodiscard]] Result<build::RunId> run();
    [[nodiscard]] Result<void> debug(std::function<void(Result<debug::DebugTarget>)> done);
    [[nodiscard]] Result<build::RunId> rerun();
    [[nodiscard]] bool running() const { return m_pending || m_running; }
    void stop();
    void shutdown();
    void input(const QString& line);

private:
    ui::BuildController& m_build;
    build::BuildService& m_sharedAdmission;
    std::shared_ptr<void> m_runAdmission;
    ui::ShellWindow& m_window;
    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    ui::RunConsole& m_console;
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
    QPointer<QAction> m_solutionAction;
    QPointer<QAction> m_runAction;
    QPointer<QAction> m_profileAction;
    QPointer<QAction> m_targetAction;
    bool m_solutionActionVisible = false;
    bool m_runActionVisible = false;
    QStringList m_expandedKeys;
    std::vector<std::pair<std::string, std::string>> m_targetPreferences;
    std::function<void(const std::vector<std::pair<std::string, std::string>>&)> m_targetPreferencesChanged;
    bool m_restoringTarget = false;
    std::uint64_t m_displayProfileGeneration = 0;
    struct RunRelay;
    std::shared_ptr<RunRelay> m_runRelay;
    std::unique_ptr<build::RunSession> m_session;
    std::string m_runTarget;
    std::function<void(Result<debug::DebugTarget>)> m_debugDone;
    bool m_pending = false;
    bool m_running = false;
    bool m_rerun = false;
    bool m_stopRequested = false;
    bool m_shutdown = false;
    std::uint64_t m_runGeneration = 0;
    std::uint64_t m_profileGeneration = 0;
    void buildFinished(std::uint64_t generation, const build::CMakeBuildOutcome& outcome);
    void maybeRerun();
    [[nodiscard]] std::optional<std::size_t> runTarget(const project::CMakeProject& model) const;
    void refreshRunTarget();
    void restoreRunTarget();
    void rememberTreeTarget();
    void refreshProfiles();
};
} // namespace frost::app
