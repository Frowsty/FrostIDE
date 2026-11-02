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
#include <string>

class QTreeView;

namespace frost::ui {
class BuildController;
class ShellWindow;
class RunConsole;
} // namespace frost::ui
namespace frost::settings {
class SettingsStore;
}
namespace frost::app {
class CMakeBuildBackend;

class CMakeIntegration final : public QObject {
public:
    CMakeIntegration(io::IFileSystem& fs, io::IProcessLauncher& launcher, project::TrustStore& trust,
                     settings::SettingsStore& settings, ui::BuildController& build, ui::ShellWindow& window,
                     ui::RunConsole& console, std::string statePath, std::function<void()> runStateChanged,
                     std::function<void(const std::string&)> openFile, std::function<void()> modelChanged);
    ~CMakeIntegration() override;
    Result<void> open(const std::string& folder);
    [[nodiscard]] build::CMakeService& service() const { return *m_service; }
    [[nodiscard]] std::shared_ptr<build::CMakeService> sharedService() const { return m_service; }
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
    QStringList m_expandedKeys;
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
};
} // namespace frost::app
