// SPDX-License-Identifier: Apache-2.0
// GUI-thread backend for a read-only compilation database or a real GNU Make project.
#pragma once
#include "frost/project/standalone_project.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include <QObject>
#include <QPointer>
class QMenu;
class QWidget;
namespace frost::ui {
class ShellWindow;
}

namespace frost::project {
class TrustStore;
}
namespace frost::app {

// Lives on the GUI thread. Session callbacks marshal through QObject, so destruction discards late
// events. The shared BuildService reservation excludes concurrent project edits/builds/runs.
class StandaloneBuildBackend final : public QObject, public ui::IBuildBackend {
public:
    StandaloneBuildBackend(project::StandaloneProject model, io::IFileSystem& fs, io::IProcessLauncher& launcher,
                           project::TrustStore& trust, build::BuildService& admission,
                           build::BuildServiceCallbacks callbacks = {},
                           std::function<Result<std::string>()> findMake = {});
    ~StandaloneBuildBackend();
    Result<void> openSolution(std::shared_ptr<const project::SolutionModel>) override;
    Result<void> closeSolution() override;
    std::shared_ptr<const project::SolutionModel> solution() const override { return {}; }
    bool hasProject() const override { return true; }
    std::vector<project::SolutionConfiguration> configurations() const override { return {}; }
    std::optional<std::size_t> activeConfigurationIndex() const override { return {}; }
    Result<void> setActiveConfiguration(std::size_t) override;
    void setSaveAllHook(build::SaveAllHook hook) override { m_save = std::move(hook); }
    Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) override;
    // Same admission/save/output path for the project menu's literal target and install actions.
    Result<void> buildTarget(project::MakeAction action, std::string target = {});
    void cancel() override;
    Result<void> trust(std::string_view root) override;
    build::BuildState state() const override { return m_state; }
    std::vector<std::string> untrustedRoots() const override;
    std::vector<build::BuildDiagnostic> diagnostics() const override { return m_diagnostics; }
    const project::StandaloneProject& model() const { return m_model; }
    void attachMenu(ui::ShellWindow& window);
    void setCallbacks(build::BuildServiceCallbacks callbacks) { m_callbacks = std::move(callbacks); }

private:
    void setState(build::BuildState state);
    void saveAndStart();
    void next();
    void finish(std::optional<Error> error = {});
    project::StandaloneProject m_model;
    io::IFileSystem& m_fs;
    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    build::BuildService& m_admission;
    build::BuildServiceCallbacks m_callbacks;
    build::SaveAllHook m_save;
    std::function<Result<std::string>()> m_findMake;
    build::BuildState m_state = build::BuildState::Idle;
    std::unique_ptr<build::BuildSession> m_session;
    std::vector<std::vector<std::string>> m_steps;
    std::vector<build::BuildDiagnostic> m_diagnostics;
    build::BuildOutcome m_outcome;
    std::size_t m_step = 0;
    bool m_reserved = false;
    bool m_cancelled = false;
    std::uint64_t m_generation = 0;
    QPointer<QMenu> m_menu;
};
} // namespace frost::app
