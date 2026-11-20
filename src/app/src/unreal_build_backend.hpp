// SPDX-License-Identifier: Apache-2.0
// GUI-thread backend for explicitly selected installed Unreal tooling, ADR0114.
#pragma once
#include "frost/project/unreal_project.hpp"
#include "frost/build/unreal_build.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>
class QMenu;
class QWidget;
class QDialog;
namespace frost::ui {
class ShellWindow;
}

namespace frost::project {
class TrustStore;
}
namespace frost::app {

// Lives on the GUI thread. Session callbacks marshal through QObject, so destruction discards late
// events. The shared BuildService reservation excludes concurrent project edits/builds/runs.
class UnrealBuildBackend final : public QObject, public ui::IBuildBackend {
public:
    UnrealBuildBackend(project::UnrealProject model, io::IFileSystem& fs, io::IProcessLauncher& launcher,
                           project::TrustStore& trust, build::BuildService& admission,
                           build::BuildServiceCallbacks callbacks = {});
    ~UnrealBuildBackend();
    Result<void> openSolution(std::shared_ptr<const project::SolutionModel>) override;
    Result<void> closeSolution() override;
    std::shared_ptr<const project::SolutionModel> solution() const override { return {}; }
    bool hasProject() const override { return true; }
    std::vector<project::SolutionConfiguration> configurations() const override { return {}; }
    std::optional<std::size_t> activeConfigurationIndex() const override { return {}; }
    Result<void> setActiveConfiguration(std::size_t) override;
    void setSaveAllHook(build::SaveAllHook hook) override { m_save = std::move(hook); }
    Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) override;
    // Same admission/save/output path for user-invoked generation and header-tool actions.
    Result<void> buildTarget(build::UnrealOperation operation);
    Result<void> configure(build::UnrealBuildRequest request);
    void cancel() override;
    Result<void> trust(std::string_view root) override;
    build::BuildState state() const override { return m_state; }
    std::vector<std::string> untrustedRoots() const override;
    std::vector<build::BuildDiagnostic> diagnostics() const override { return m_diagnostics; }
    const project::UnrealProject& model() const { return m_model; }
    void attachMenu(ui::ShellWindow& window, std::function<Result<void>(std::string)> addEditor = {},
        std::function<void(const std::string&)> openFile = {}, std::function<void()> semanticProfileChanged = {});
    Result<void> selectCompileDatabase(std::string file);
    const std::string& compileDatabase() const { return m_databaseFile; }
    std::string configurationIdentity() const;
    const std::string& visualizer() const { return m_visualizer; }
    void setCallbacks(build::BuildServiceCallbacks callbacks) { m_callbacks = std::move(callbacks); }

private:
    void showProjectModel(QWidget* host, std::function<void(const std::string&)> openFile);
    void setState(build::BuildState state);
    bool admitted() const;
    void checkTrustRevision();
    void saveAndStart();
    void next();
    void finish(std::optional<Error> error = {});
    project::UnrealProject m_model;
    std::string m_root;
    std::string m_visualizer;
    std::string m_databaseFile;
    std::function<void()> m_semanticProfileChanged;
    std::optional<project::UnrealWorkspace> m_workspace;
    QPointer<QDialog> m_modelDialog;
    build::UnrealBuildRequest m_request;
    io::IFileSystem& m_fs;
    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    build::BuildService& m_admission;
    build::BuildServiceCallbacks m_callbacks;
    build::SaveAllHook m_save;
    build::BuildState m_state = build::BuildState::Idle;
    std::unique_ptr<build::BuildSession> m_session;
    std::vector<io::ProcessSpec> m_steps;
    std::vector<build::BuildDiagnostic> m_diagnostics;
    build::BuildOutcome m_outcome;
    std::size_t m_step = 0;
    bool m_reserved = false;
    bool m_cancelled = false;
    std::uint64_t m_generation = 0;
    QPointer<QMenu> m_menu;
    QTimer m_trustWatch;
    std::optional<std::uint64_t> m_trustRevision;
};
} // namespace frost::app
