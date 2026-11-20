// SPDX-License-Identifier: Apache-2.0
// Selected SDK context's normal Build/Problems backend; process work remains on the scheduler.
#pragma once
#include "frost/build/managed_evaluation.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include <QObject>
namespace frost::app {
struct ManagedBuildJob;
class ManagedBuildBackend final : public QObject, public ui::IBuildBackend {
public:
    ManagedBuildBackend(std::shared_ptr<const build::ManagedProjectEvaluation>, std::string root,
                        std::uint64_t trustRevision, io::IFileSystem&, io::IProcessLauncher&, project::TrustStore&,
                        build::BuildService&, sched::ThreadPool&, std::vector<sched::JoinHandle<void>>&,
                        build::BuildServiceCallbacks);
    ~ManagedBuildBackend();
    Result<void> openSolution(std::shared_ptr<const project::SolutionModel>) override {
        return fail(ErrorCode::Unsupported, "An evaluated SDK context is selected");
    }
    Result<void> closeSolution() override;
    std::shared_ptr<const project::SolutionModel> solution() const override { return {}; }
    bool hasProject() const override { return true; }
    std::vector<project::SolutionConfiguration> configurations() const override;
    std::optional<std::size_t> activeConfigurationIndex() const override { return 0; }
    Result<void> setActiveConfiguration(std::size_t index) override;
    void setSaveAllHook(build::SaveAllHook hook) override { m_save = std::move(hook); }
    Result<void> build(build::BuildAction, std::optional<project::ProjectId>) override;
    void cancel() override;
    Result<void> trust(std::string_view) override;
    build::BuildState state() const override { return m_state; }
    std::vector<std::string> untrustedRoots() const override;
    std::vector<build::BuildDiagnostic> diagnostics() const override { return m_diagnostics; }
    void finish(build::BuildOutcome);
    void diagnostic(build::BuildDiagnostic);

private:
    void setState(build::BuildState);
    void start(build::BuildAction, std::uint64_t);
    std::shared_ptr<const build::ManagedProjectEvaluation> m_model;
    std::string m_root;
    std::uint64_t m_trustRevision;
    io::IFileSystem& m_fs;
    io::IProcessLauncher& m_launcher;
    project::TrustStore& m_trust;
    build::BuildService& m_admission;
    sched::ThreadPool& m_pool;
    std::vector<sched::JoinHandle<void>>& m_tasks;
    build::BuildServiceCallbacks m_callbacks;
    build::SaveAllHook m_save;
    std::shared_ptr<ManagedBuildJob> m_job;
    build::BuildState m_state = build::BuildState::Idle;
    std::vector<build::BuildDiagnostic> m_diagnostics;
    std::uint64_t m_generation = 0;
};
} // namespace frost::app
