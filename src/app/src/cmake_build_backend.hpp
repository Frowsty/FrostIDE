// SPDX-License-Identifier: Apache-2.0
// The build tool windows' backend for a CMake folder.
#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

namespace frost::app {

// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor): Final concrete owner; base destruction is protected.
class CMakeBuildBackend final : public ui::IBuildBackend {
public:
    explicit CMakeBuildBackend(build::CMakeService& service) : m_service(service) {}
    ~CMakeBuildBackend() = default;
    Result<void> openSolution(std::shared_ptr<const project::SolutionModel>) override {
        return fail(ErrorCode::Unsupported, "a CMake folder is open");
    }
    Result<void> closeSolution() override { return m_service.closeFolder(); }
    std::shared_ptr<const project::SolutionModel> solution() const override { return {}; }
    bool hasProject() const override { return !m_service.sourceDirectory().empty(); }
    std::vector<project::SolutionConfiguration> configurations() const override { return {}; }
    std::optional<std::size_t> activeConfigurationIndex() const override { return {}; }
    Result<void> setActiveConfiguration(std::size_t) override {
        return fail(ErrorCode::Unsupported, "select a CMake profile instead");
    }
    void setSaveAllHook(build::SaveAllHook hook) override { m_service.setSaveAllHook(std::move(hook)); }
    Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) override {
        if (project) {
            return fail(ErrorCode::InvalidArgument, "a solution project cannot be built in a CMake folder");
        }
        return m_service.build(action);
    }
    void cancel() override { m_service.cancel(); }
    Result<void> compileFile(std::string_view path) override { return m_service.compileFile(path); }
    bool canCompile(std::string_view path) const override { return m_service.canCompileFile(path); }
    Result<void> trust(std::string_view root) override { return m_service.trust(root); }
    build::BuildState state() const override { return m_service.state(); }
    std::vector<std::string> untrustedRoots() const override { return m_service.untrustedRoots(); }
    std::vector<build::BuildDiagnostic> diagnostics() const override { return m_service.diagnostics(); }

private:
    build::CMakeService& m_service;
};

// Fan out the callbacks shared by the CMake and build tool windows. Both relays marshal to Qt.
inline build::CMakeServiceCallbacks cmakeBuildCallbacks(build::CMakeServiceCallbacks cmake,
                                                        build::BuildServiceCallbacks forwardBuild) {
    auto state = std::move(cmake.onStateChanged);
    cmake.onStateChanged = [state = std::move(state), forward = std::move(forwardBuild.onStateChanged)](auto value) {
        if (state)
            state(value);
        if (forward)
            forward(value);
    };
    cmake.onNeedsTrust = std::move(forwardBuild.onNeedsTrust);
    cmake.onOutput = std::move(forwardBuild.onOutput);
    cmake.onDiagnostic = std::move(forwardBuild.onDiagnostic);
    cmake.onOutputFenced = std::move(forwardBuild.onOutputFenced);
    cmake.onDiagnosticFenced = std::move(forwardBuild.onDiagnosticFenced);
    cmake.onDiagnosticsCleared = std::move(forwardBuild.onDiagnosticsCleared);
    cmake.onFinished = [forward = std::move(forwardBuild.onFinished)](const build::CMakeBuildOutcome& outcome) {
        if (forward) {
            build::BuildOutcome translated;
            translated.target.action = outcome.request.action;
            translated.target.projectName = outcome.request.targetName;
            translated.target.file = outcome.request.file;
            translated.result = outcome.result;
            translated.error = outcome.error;
            translated.counts = outcome.counts;
            forward(translated);
        }
    };
    if (forwardBuild.onFinishedFenced)
        cmake.onFinishedFenced = [forward = std::move(forwardBuild.onFinishedFenced)](
                                     const build::CMakeBuildOutcome& outcome, std::function<bool()> fence) {
            if (forward) {
                build::BuildOutcome translated;
                translated.target.action = outcome.request.action;
                translated.target.projectName = outcome.request.targetName;
                translated.target.file = outcome.request.file;
                translated.result = outcome.result;
                translated.error = outcome.error;
                translated.counts = outcome.counts;
                forward(translated, std::move(fence));
            }
        };
    return cmake;
}

} // namespace frost::app
