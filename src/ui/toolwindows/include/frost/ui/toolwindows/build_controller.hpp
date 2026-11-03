// SPDX-License-Identifier: Apache-2.0
//
// BuildController: the GUI thread's side of the build service (W1, ADR 0039). The build tool windows
// (the Solution Explorer's build commands, the build output, the Problems list, the configuration
// combo, the Build actions and the trust prompt) talk to it instead of frost::build::BuildService,
// whose callbacks run on the service's own thread.
//
/*
 * Split. IBuildBackend is the plain C++ seam over the requests and queries the UI needs;
 * BuildServiceBackend adapts frost::build::BuildService to it and tests substitute a fake.
 * BuildController is a QObject on the GUI thread: it forwards requests to the backend and re-emits
 * the service's events as Qt signals.
 *
 * Wiring (the shell's composition root):
 *     auto* controller = new BuildController(window);
 *     auto service = BuildService::create(fs, launcher, trust, options, controller->serviceCallbacks());
 *     BuildServiceBackend backend(*service);
 *     controller->setBackend(&backend);
 * The service must be destroyed before the backend; the controller may go first or last.
 *
 * Marshalling. Each callback of serviceCallbacks() copies its arguments and posts them to the
 * controller with QMetaObject::invokeMethod(Qt::QueuedConnection), so every signal is emitted on
 * the GUI thread, one at a time, in the order the service reported the events. The callbacks reach
 * the controller through a shared relay whose pointer the controller's destructor clears under a
 * mutex: a callback racing the destruction posts nothing, and events posted before it are dropped
 * by Qt with the object.
 *
 * Cached state. The controller keeps what the events told it (state, untrusted folders, the
 * current build's diagnostics and output lines, progress, the last outcome) and the solution's
 * configurations, so widgets read them on the GUI thread without locking, and a tool window created
 * in the middle of a build replays what it missed before following the signals.
 *
 * Save All. setBackend installs a Save-All hook: on the service's thread it posts to the GUI thread,
 * which calls the ISaveAllHandler (if any; none succeeds at once) and passes its completion back.
 * Once the controller is gone the hook fails with Cancelled.
 *
 * Threads: every member except serviceCallbacks() is GUI-thread only.
 */
#pragma once

#include "frost/build/build_service.hpp"
#include "frost/core/error.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/ui/toolwindows/save_all_handler.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

// What the build tool windows ask of the build service (frost/build/build_service.hpp documents each
// call). Implementations must be callable from the GUI thread; they need not be thread-safe beyond that.
class IBuildBackend {
public:
    [[nodiscard]] virtual Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution) = 0;
    [[nodiscard]] virtual Result<void> closeSolution() = 0;
    [[nodiscard]] virtual std::shared_ptr<const project::SolutionModel> solution() const = 0;
    // A backend may represent a non-solution project, such as a CMake folder.
    [[nodiscard]] virtual bool hasProject() const { return solution() != nullptr; }
    [[nodiscard]] virtual std::vector<project::SolutionConfiguration> configurations() const = 0;
    [[nodiscard]] virtual std::optional<std::size_t> activeConfigurationIndex() const = 0;
    [[nodiscard]] virtual Result<void> setActiveConfiguration(std::size_t index) = 0;
    virtual void setSaveAllHook(build::SaveAllHook hook) = 0;
    [[nodiscard]] virtual Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) = 0;
    virtual void cancel() = 0;
    [[nodiscard]] virtual Result<void> trust(std::string_view root) = 0;
    [[nodiscard]] virtual build::BuildState state() const = 0;
    [[nodiscard]] virtual std::vector<std::string> untrustedRoots() const = 0;
    [[nodiscard]] virtual std::vector<build::BuildDiagnostic> diagnostics() const = 0;

protected:
    IBuildBackend() = default;
    IBuildBackend(const IBuildBackend&) = default;
    IBuildBackend& operator=(const IBuildBackend&) = default;
    ~IBuildBackend() = default;
};

// IBuildBackend over a frost::build::BuildService, which must outlive it. Every call forwards.
class BuildServiceBackend final : public IBuildBackend {
public:
    explicit BuildServiceBackend(build::BuildService& service) noexcept : m_service(service) {}

    [[nodiscard]] Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution) override;
    [[nodiscard]] Result<void> closeSolution() override;
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const override;
    [[nodiscard]] std::vector<project::SolutionConfiguration> configurations() const override;
    [[nodiscard]] std::optional<std::size_t> activeConfigurationIndex() const override;
    [[nodiscard]] Result<void> setActiveConfiguration(std::size_t index) override;
    void setSaveAllHook(build::SaveAllHook hook) override;
    [[nodiscard]] Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project) override;
    void cancel() override;
    [[nodiscard]] Result<void> trust(std::string_view root) override;
    [[nodiscard]] build::BuildState state() const override;
    [[nodiscard]] std::vector<std::string> untrustedRoots() const override;
    [[nodiscard]] std::vector<build::BuildDiagnostic> diagnostics() const override;

private:
    build::BuildService& m_service;
};

// One output line of the current build, as the controller keeps it for late subscribers.
struct BuildOutputEntry {
    build::BuildOutputKind kind = build::BuildOutputKind::Stdout;
    QString text;
};

class BuildController final : public QObject {
    Q_OBJECT

public:
    // The most output lines kept for replay; older ones are dropped (the signals still carry them all).
    static constexpr std::size_t kMaxReplayLines = 200'000;

    explicit BuildController(QObject* parent = nullptr);
    ~BuildController() override;

    // The callbacks to create the service with (header comment). Copies may be called from any thread,
    // also after the controller is gone (they then do nothing).
    // `active`, when supplied, is checked on the GUI thread before delivering each event.
    [[nodiscard]] build::BuildServiceCallbacks serviceCallbacks(std::function<bool()> active = {}) const;

    /*
     * Binds `backend` (not owned; it must outlive the controller or be replaced first), installs the
     * Save-All hook on it, reads its solution, configurations, state and diagnostics, and emits
     * solutionChanged(). nullptr unbinds: requests then fail with InvalidArgument.
     */
    void setBackend(IBuildBackend* backend);
    [[nodiscard]] IBuildBackend* backend() const noexcept { return m_backend; }

    // The Save All to run before each build (not owned; must outlive the controller or be reset).
    void setSaveAllHandler(ISaveAllHandler* handler) noexcept { m_saveAll = handler; }

    /*
     * Opens or closes the solution through the backend, then refreshes the configurations and emits
     * solutionChanged() and activeConfigurationChanged(). Errors: those of the backend
     * (BuildService::openSolution: InvalidArgument, Busy); InvalidArgument without a backend.
     */
    [[nodiscard]] Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution);
    [[nodiscard]] Result<void> closeSolution();
    // Publishes the backend's already-installed model after a reserved project edit.
    void projectEditPublished();
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const { return m_solution; }
    [[nodiscard]] bool hasSolution() const noexcept { return m_solution != nullptr; }

    // The solution's configurations ("Debug|x64") in solution order and the active one's index.
    [[nodiscard]] const std::vector<project::SolutionConfiguration>& configurations() const noexcept {
        return m_configurations;
    }
    [[nodiscard]] std::optional<std::size_t> activeConfigurationIndex() const noexcept { return m_active; }
    // Activates and persists configurations()[index]; emits activeConfigurationChanged() on success.
    // Errors: those of BuildService::setActiveConfiguration (Busy during a build, OutOfRange, file errors).
    [[nodiscard]] Result<void> setActiveConfiguration(std::size_t index);

    /*
     * Starts `action` for the solution or one project (BuildService::build). A refused request also
     * emits requestFailed() with the reason, so callers that only fire a command may ignore the result.
     */
    [[nodiscard]] Result<void> build(build::BuildAction action, std::optional<project::ProjectId> project = {});
    void cancel();
    // Trusts one folder (BuildService::trust); a failure also emits requestFailed().
    [[nodiscard]] Result<void> trust(std::string_view root);

    [[nodiscard]] build::BuildState state() const noexcept { return m_state; }
    // A build is anywhere between its request and Idle.
    [[nodiscard]] bool isBuilding() const noexcept { return m_state != build::BuildState::Idle; }
    // Build commands may start: a solution is open and no build runs.
    [[nodiscard]] bool canBuild() const noexcept { return m_hasProject && !isBuilding(); }
    [[nodiscard]] const std::vector<std::string>& untrustedRoots() const noexcept { return m_untrusted; }
    [[nodiscard]] const std::vector<build::BuildDiagnostic>& diagnostics() const noexcept { return m_diagnostics; }
    [[nodiscard]] const std::deque<BuildOutputEntry>& currentOutput() const noexcept { return m_output; }
    [[nodiscard]] const build::BuildProgress& progress() const noexcept { return m_progress; }
    [[nodiscard]] const std::optional<build::BuildOutcome>& lastOutcome() const noexcept { return m_lastOutcome; }

signals:
    void backendChanged();
    void solutionChanged();
    void activeConfigurationChanged(int index);
    void stateChanged(frost::build::BuildState state);
    // A build left Idle: its output and diagnostics start afresh.
    void buildStarted();
    void needsTrust(const QStringList& untrustedRoots);
    void outputLine(frost::build::BuildOutputKind kind, const QString& text);
    void diagnosticAdded(const frost::build::BuildDiagnostic& diagnostic);
    void diagnosticsCleared();
    void progressChanged(const frost::build::BuildProgress& progress);
    void buildFinished(const frost::build::BuildOutcome& outcome);
    // A request (build, trust, configuration switch) was refused; for the shell's notifications.
    void requestFailed(const QString& message);

private:
    struct Relay;

    void refreshFromBackend();
    void reportFailure(const Error& error);
    void runSaveAll(const build::SaveAllCompletion& done);
    void applyState(build::BuildState state);
    void applyNeedsTrust(std::vector<std::string> roots);
    void applyOutput(build::BuildOutputKind kind, const std::string& line);
    void applyDiagnostic(const build::BuildDiagnostic& diagnostic);
    void applyDiagnosticsCleared();
    void applyProgress(const build::BuildProgress& progress);
    void applyFinished(const build::BuildOutcome& outcome);
    void applyActiveConfiguration(const project::SolutionConfiguration& configuration);

    std::shared_ptr<Relay> m_relay;
    IBuildBackend* m_backend = nullptr;
    ISaveAllHandler* m_saveAll = nullptr;
    std::shared_ptr<const project::SolutionModel> m_solution;
    bool m_hasProject = false;
    std::vector<project::SolutionConfiguration> m_configurations;
    std::optional<std::size_t> m_active;
    build::BuildState m_state = build::BuildState::Idle;
    std::vector<std::string> m_untrusted;
    std::vector<build::BuildDiagnostic> m_diagnostics;
    std::deque<BuildOutputEntry> m_output;
    build::BuildProgress m_progress;
    std::optional<build::BuildOutcome> m_lastOutcome;
};

// "Debug | x64": how the tool windows show a solution configuration.
[[nodiscard]] QString configurationLabel(const project::SolutionConfiguration& configuration);

} // namespace frost::ui
